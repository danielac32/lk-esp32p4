/*
 * Cargador de apps ELF ET_REL RV32 para LK/ESP32-P4.
 * Portado/adaptado de system/elfload.c de Xinu (P4): misma logica de carga,
 * colocacion en PSRAM (ventana directa 0x88), resolucion contra exports del
 * kernel, relocaciones RISC-V y ejecucion en el hilo llamador. Adaptado a las
 * APIs de LK (lib/fs, psram_malloc, hilos LK, cache ESP32-P4).
 */
#include <lib/elfload.h>
#include <lib/elf_exports.h>

#include <lk/debug.h>
#include <lk/err.h>
#include <kernel/thread.h>
#include <kernel/spinlock.h>
#include <lib/fs.h>
#include <platform/psram_heap.h>
#include <arch/ops.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---- Definiciones ELF minimas (compatibles con el formato usado por Xinu) ---- */
typedef uint32_t elf32_addr;
typedef uint32_t elf32_off;
typedef uint16_t elf32_half;
typedef uint32_t elf32_word;
typedef int32_t  elf32_sword;

typedef struct {
    uint8_t    e_ident[16];
    elf32_half e_type;
    elf32_half e_machine;
    elf32_word e_version;
    elf32_addr e_entry;
    elf32_off  e_phoff;
    elf32_off  e_shoff;
    elf32_word e_flags;
    elf32_half e_ehsize;
    elf32_half e_phentsize;
    elf32_half e_phnum;
    elf32_half e_shentsize;
    elf32_half e_shnum;
    elf32_half e_shstrndx;
} ELF32_hdr;

typedef struct {
    elf32_word sh_name;
    elf32_word sh_type;
    elf32_word sh_flags;
    elf32_addr sh_addr;
    elf32_off  sh_off;
    elf32_word sh_size;
    elf32_word sh_link;
    elf32_word sh_info;
    elf32_word sh_addralign;
    elf32_word sh_entsize;
} ELF32_shdr;

typedef struct {
    elf32_word    st_name;
    elf32_addr    st_value;
    elf32_word    st_size;
    unsigned char st_info;
    unsigned char st_other;
    elf32_half    st_shndx;
} Elf32_Sym;

typedef struct {
    elf32_addr  r_offset;
    elf32_word  r_info;
    elf32_sword r_addend;
} Elf32_Rela;

#define EI_CLASS 4
#define EC_32    1
#define EM_RISCV 0xF3
#define ET_REL   1

#define SHT_NULL    0
#define SHT_PROGBITS 1
#define SHT_SYMTAB  2
#define SHT_STRTAB  3
#define SHT_RELA    4
#define SHT_NOBITS  8

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXEC  0x4

#define SHN_UNDEF 0
#define SHN_ABS   0xfff1

#define ELF32_R_SYM(i)  ((i) >> 8)
#define ELF32_R_TYPE(i) ((i) & 0xFF)

#define R_RISCV_32       1
#define R_RISCV_CALL     18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_HI20     26
#define R_RISCV_LO12_I   27

#define PSRAM_DIRECT    0x40000000U
#define ALINEA_MIN      4U
#define ELF_DETALLE_MAX 12

/* ---- Cache sync ESP32-P4 (registros CACHE, base 0x3FF10000) ---- */
#define P4_CACHE_BASE      0x3FF10000UL
#define P4_CACHE_SYNC_CTRL (*(volatile uint32_t *)(P4_CACHE_BASE + 0x98))
#define P4_CACHE_SYNC_MAP  (*(volatile uint32_t *)(P4_CACHE_BASE + 0x9c))
#define P4_CACHE_SYNC_ADDR (*(volatile uint32_t *)(P4_CACHE_BASE + 0xa0))
#define P4_CACHE_SYNC_SIZE (*(volatile uint32_t *)(P4_CACHE_BASE + 0xa4))

#define LINEA_CACHE  64U
#define SYNC_MAP_L1D 0x10U
#define SYNC_MAP_L2  0x20U
#define SYNC_INVAL   0x01U
#define SYNC_WBINVAL 0x08U

static spin_lock_t cache_sync_lock = SPIN_LOCK_INITIAL_VALUE;

static void elf_cache_sync(uintptr_t addr, uintptr_t size, uint32_t op) {
    if (size == 0) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&cache_sync_lock);

    addr &= ~(uintptr_t)(LINEA_CACHE - 1U);
    uintptr_t fin = (addr + size + (LINEA_CACHE - 1U)) & ~(uintptr_t)(LINEA_CACHE - 1U);

    P4_CACHE_SYNC_MAP  = SYNC_MAP_L1D | SYNC_MAP_L2;
    P4_CACHE_SYNC_ADDR = (uint32_t)addr;
    P4_CACHE_SYNC_SIZE = (uint32_t)(fin - addr);
    P4_CACHE_SYNC_CTRL = op;

    for (unsigned intentos = 0; intentos < 5000000U; intentos++) {
        if ((P4_CACHE_SYNC_CTRL & op) == 0) {
            spin_unlock_irqrestore(&cache_sync_lock, state);
            return;
        }
    }
    spin_unlock_irqrestore(&cache_sync_lock, state);
    printf("elfload: AVISO: operacion de cache 0x%x no termino\n", (unsigned)op);
}

/* ---- Contexto de imagen ELF por hilo ---- */
struct elfimg {
    void *bloque;           /* psram_malloc (cached) */
    unsigned long tam;      /* bytes de secciones colocadas */
    unsigned int entrada;   /* _start en ventana directa */
    char nombre[ELF_MAX_NOMBRE];
};

struct elf_thread_ctx {
    thread_t *t;
    struct elfimg *img;
    bool active;
    int exit_code;
    bool exit_called;
};

#define MAX_ELF_CTX 8
static struct elf_thread_ctx elf_ctxs[MAX_ELF_CTX];
static spin_lock_t elf_ctx_lock = SPIN_LOCK_INITIAL_VALUE;

static struct elf_thread_ctx *elf_ctx_alloc(thread_t *t, struct elfimg *img) {
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&elf_ctx_lock);
    for (int i = 0; i < MAX_ELF_CTX; i++) {
        if (!elf_ctxs[i].active) {
            elf_ctxs[i].t = t;
            elf_ctxs[i].img = img;
            elf_ctxs[i].active = true;
            elf_ctxs[i].exit_code = 0;
            elf_ctxs[i].exit_called = false;
            spin_unlock_irqrestore(&elf_ctx_lock, state);
            return &elf_ctxs[i];
        }
    }
    spin_unlock_irqrestore(&elf_ctx_lock, state);
    return NULL;
}

static struct elf_thread_ctx *elf_ctx_find(thread_t *t) {
    for (int i = 0; i < MAX_ELF_CTX; i++) {
        if (elf_ctxs[i].active && elf_ctxs[i].t == t) return &elf_ctxs[i];
    }
    return NULL;
}

static void elf_ctx_free(struct elf_thread_ctx *ctx) {
    if (!ctx) return;
    arch_interrupt_saved_state_t state = spin_lock_irqsave(&elf_ctx_lock);
    ctx->active = false;
    ctx->t = NULL;
    ctx->img = NULL;
    spin_unlock_irqrestore(&elf_ctx_lock, state);
}

const char *elf_current_name(void) {
    thread_t *t = get_current_thread();
    struct elf_thread_ctx *ctx = elf_ctx_find(t);
    if (ctx && ctx->img) return ctx->img->nombre;
    return NULL;
}

/* ---- exit exportado para apps ELF ---- */
static void elf_app_exit(int code) {
    thread_t *t = get_current_thread();
    struct elf_thread_ctx *ctx = elf_ctx_find(t);
    printf("elf: exit(%d)\n", code);
    if (ctx && ctx->img) {
        ctx->exit_code = code;
        ctx->exit_called = true;
        /* liberar imagen antes de terminar el hilo */
        if (ctx->img->bloque) {
            psram_free(ctx->img->bloque);
            ctx->img->bloque = NULL;
        }
        elf_ctx_free(ctx);
    }
    thread_exit(code);
    for (;;) {}
}

static void *elf_resolve(const char *name) {
    if (strcmp(name, "exit") == 0) return (void *)elf_app_exit;
    return elf_export_resolve(name);
}

/* ---- parches de inmediato RISC-V ---- */
static unsigned int elf_patch_hi20(unsigned int instr, unsigned int valor) {
    return (instr & 0x00000FFFU) | ((valor + 0x800U) & 0xFFFFF000U);
}
static unsigned int elf_patch_lo12(unsigned int instr, unsigned int valor) {
    return (instr & 0x000FFFFFU) | ((valor & 0xFFFU) << 20);
}

static void elf_fracaso(struct elfimg *img, unsigned int *tablas, uintptr_t cached) {
    if (tablas) psram_free(tablas);
    if (cached) psram_free((void *)cached);
    if (img) {
        img->bloque = NULL;
        img->entrada = 0;
        img->tam = 0;
    }
}

/* ---- carga, enlace y reubicacion ---- */
static int elf_load_image(const void *imagen, size_t len, const char *etiqueta, struct elfimg *img) {
    const ELF32_hdr *hdr;
    const ELF32_shdr *sh, *shstr, *symtab = NULL, *strtabsec;
    const Elf32_Sym *sym;
    const Elf32_Rela *rela;
    const char *shstrtab, *strtab, *nombre;
    unsigned int *tablas = NULL, *base, *simval;
    uintptr_t cursor, total = 0, fin, inicio, directo, cached, reserva;
    unsigned int nsec, nsym, relocs = 0, parches = 0;
    unsigned int ntexto = 0, ndatos = 0, bytes_texto = 0, bytes_datos = 0;
    unsigned int start = 0;

    if (!imagen || !img || len == 0) return -1;
    img->bloque = NULL;
    img->entrada = 0;
    img->tam = 0;
    snprintf(img->nombre, sizeof(img->nombre), "%s", etiqueta ? etiqueta : "elf");

    hdr = (const ELF32_hdr *)imagen;
    if (hdr->e_ident[0] != 0x7F || hdr->e_ident[1] != 'E' || hdr->e_ident[2] != 'L' || hdr->e_ident[3] != 'F') {
        printf("elfload: %s no es ELF\n", img->nombre);
        return -1;
    }
    if (hdr->e_ident[EI_CLASS] != EC_32 || hdr->e_machine != EM_RISCV) {
        printf("elfload: %s no es RV32 (class=%u machine=0x%x)\n", img->nombre, hdr->e_ident[EI_CLASS], hdr->e_machine);
        return -1;
    }
    if (hdr->e_type != ET_REL) {
        printf("elfload: %s e_type=%u, se requiere ET_REL (ld -r)\n", img->nombre, hdr->e_type);
        return -1;
    }

    nsec = hdr->e_shnum;
    if (nsec == 0 || nsec > ELF_MAX_SECC) {
        printf("elfload: %s tiene %u secciones (tope %u)\n", img->nombre, nsec, ELF_MAX_SECC);
        return -1;
    }

    sh = (const ELF32_shdr *)((const uint8_t *)hdr + hdr->e_shoff);
    shstr = &sh[hdr->e_shstrndx];
    shstrtab = (const char *)((const uint8_t *)hdr + shstr->sh_off);

    for (unsigned i = 0; i < nsec; i++) {
        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0) continue;
        uintptr_t al = sh[i].sh_addralign;
        if (al < ALINEA_MIN) al = ALINEA_MIN;
        total = (total + al - 1) & ~(al - 1);
        total += sh[i].sh_size;
    }
    total += 64U;

    /* Reservar margen para no pisar metadatos del heap (dlmalloc guarda
     * cabecera antes del puntero devuelto) y para alinear a linea de cache. */
    reserva = total + 4U * LINEA_CACHE;
    cached = (uintptr_t)psram_malloc(reserva);
    if (cached == 0) {
        printf("elfload: %s necesita %lu bytes de PSRAM\n", img->nombre, (unsigned long)reserva);
        return -1;
    }
    /* Inicio alineado a linea de cache, dejando al menos una linea libre
     * tras el bloque devuelto por malloc para no invalidar su cabecera. */
    inicio = (cached + 2U * LINEA_CACHE - 1U) & ~(uintptr_t)(LINEA_CACHE - 1U);
    if (inicio < cached + LINEA_CACHE) inicio += LINEA_CACHE;
    directo = inicio + PSRAM_DIRECT;

    elf_cache_sync(inicio, total, SYNC_WBINVAL);

    for (unsigned i = 0; i < nsec; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) { symtab = &sh[i]; break; }
    }
    if (!symtab) {
        printf("elfload: %s no tiene .symtab\n", img->nombre);
        elf_fracaso(img, NULL, cached);
        return -1;
    }
    strtabsec = &sh[symtab->sh_link];
    nsym = (unsigned int)(symtab->sh_size / sizeof(Elf32_Sym));
    if (nsym == 0 || nsym > ELF_MAX_SIMB) {
        printf("elfload: %s tiene %u simbolos (tope %u)\n", img->nombre, nsym, ELF_MAX_SIMB);
        elf_fracaso(img, NULL, cached);
        return -1;
    }
    if (strtabsec->sh_type != SHT_STRTAB) {
        printf("elfload: %s .symtab sin .strtab valido\n", img->nombre);
        elf_fracaso(img, NULL, cached);
        return -1;
    }

    tablas = (unsigned int *)psram_malloc((nsec + nsym) * sizeof(unsigned int));
    if (!tablas) {
        printf("elfload: %s sin memoria para tablas\n", img->nombre);
        elf_fracaso(img, NULL, cached);
        return -1;
    }
    base = tablas;
    simval = tablas + nsec;
    memset(tablas, 0, (nsec + nsym) * sizeof(unsigned int));

    sym = (const Elf32_Sym *)((const uint8_t *)hdr + symtab->sh_off);
    strtab = (const char *)((const uint8_t *)hdr + strtabsec->sh_off);

    printf("elfload: %s: %u secciones, %u simbolos, %lu bytes, bloque=0x%08lx directo=0x%08lx\n",
           img->nombre, nsec, nsym, (unsigned long)len, (unsigned long)cached, (unsigned long)directo);

    cursor = directo;
    for (unsigned i = 0; i < nsec; i++) {
        if ((sh[i].sh_flags & SHF_ALLOC) == 0 || sh[i].sh_size == 0) { base[i] = 0; continue; }
        uintptr_t al = sh[i].sh_addralign;
        if (al < ALINEA_MIN) al = ALINEA_MIN;
        cursor = (cursor + al - 1) & ~(al - 1);
        base[i] = (unsigned int)cursor;

        if (sh[i].sh_type == SHT_NOBITS) {
            memset((void *)(uintptr_t)base[i], 0, sh[i].sh_size);
        } else {
            memcpy((void *)(uintptr_t)base[i], (const uint8_t *)hdr + sh[i].sh_off, sh[i].sh_size);
        }
        cursor += sh[i].sh_size;

        if (sh[i].sh_flags & SHF_EXEC) { ntexto++; bytes_texto += (unsigned)sh[i].sh_size; }
        else { ndatos++; bytes_datos += (unsigned)sh[i].sh_size; }

        if (i < ELF_DETALLE_MAX || (sh[i].sh_flags & SHF_EXEC) == 0) {
            nombre = shstrtab + sh[i].sh_name;
            printf("   [%s] %-22s 0x%08x %u bytes%s\n",
                   (sh[i].sh_flags & SHF_EXEC) ? "texto" : "datos",
                   nombre, base[i], (unsigned)sh[i].sh_size,
                   (sh[i].sh_type == SHT_NOBITS) ? " (bss)" : "");
        }
    }
    if (ntexto > ELF_DETALLE_MAX) {
        printf("   ... y %u secciones de texto mas\n", ntexto - ELF_DETALLE_MAX);
    }
    fin = cursor;

    /* 3. Simbolos */
    for (unsigned i = 1; i < nsym; i++) {
        simval[i] = 0;
        if (sym[i].st_shndx == SHN_UNDEF) {
            void *d = elf_resolve(strtab + sym[i].st_name);
            if (!d) {
                printf("elfload: %s: simbolo SIN EXPORTAR: %s\n", img->nombre, strtab + sym[i].st_name);
                elf_fracaso(img, tablas, cached);
                return -1;
            }
            simval[i] = (unsigned int)(uintptr_t)d;
        } else if (sym[i].st_shndx == SHN_ABS) {
            simval[i] = (unsigned int)sym[i].st_value;
        } else if (sym[i].st_shndx < nsec && base[sym[i].st_shndx] != 0) {
            simval[i] = base[sym[i].st_shndx] + (unsigned int)sym[i].st_value;
        }
    }

    /* 4. Relocaciones */
    for (unsigned i = 0; i < nsec; i++) {
        if (sh[i].sh_type != SHT_RELA) continue;
        unsigned tgt = sh[i].sh_info;
        if (tgt >= nsec || base[tgt] == 0) {
            printf("elfload: %s: %s no apunta a seccion colocada\n", img->nombre, shstrtab + sh[i].sh_name);
            elf_fracaso(img, tablas, cached);
            return -1;
        }
        rela = (const Elf32_Rela *)((const uint8_t *)hdr + sh[i].sh_off);
        unsigned nrela = (unsigned)(sh[i].sh_size / sizeof(Elf32_Rela));
        for (unsigned j = 0; j < nrela; j++) {
            unsigned int sitio = base[tgt] + (unsigned int)rela[j].r_offset;
            unsigned int sidx  = ELF32_R_SYM(rela[j].r_info);
            unsigned int tipo  = ELF32_R_TYPE(rela[j].r_info);
            unsigned int A     = (unsigned int)rela[j].r_addend;
            unsigned int S     = (sidx < nsym) ? simval[sidx] : 0U;
            unsigned int *pal  = (unsigned int *)(uintptr_t)sitio;

            if (sitio + 4U > fin || (sitio & 3U) != 0U) {
                printf("elfload: %s: reloc fuera de %s (0x%08x)\n", img->nombre, shstrtab + sh[tgt].sh_name, sitio);
                elf_fracaso(img, tablas, cached);
                return -1;
            }
            relocs++;
            switch (tipo) {
                case R_RISCV_HI20:
                    *pal = elf_patch_hi20(*pal, S + A);
                    parches++;
                    break;
                case R_RISCV_LO12_I:
                    *pal = elf_patch_lo12(*pal, S + A);
                    parches++;
                    break;
                case R_RISCV_32:
                    *pal = S + A;
                    parches++;
                    break;
                case R_RISCV_CALL:
                case R_RISCV_CALL_PLT: {
                    unsigned int diff = (unsigned int)((int)(S + A) - (int)sitio);
                    *pal = elf_patch_hi20(*pal, diff);
                    unsigned int hi = (diff + 0x800U) & 0xFFFFF000U;
                    unsigned int lo = diff - hi;
                    unsigned int *pal2 = (unsigned int *)(uintptr_t)(sitio + 4U);
                    *pal2 = elf_patch_lo12(*pal2, lo);
                    /* verificacion */
                    unsigned int w0 = *pal;
                    unsigned int w1 = *pal2;
                    long h = (long)(w0 & 0xFFFFF000U);
                    long l = (long)(w1 >> 20);
                    l = (l ^ 0x800L) - 0x800L;
                    if ((unsigned long)(sitio + (unsigned long)h + (unsigned long)l) != (unsigned long)(S + A)) {
                        const char *sname = (sidx < nsym) ? (strtab + sym[sidx].st_name) : "?";
                        printf("elfload: CALL no alcanza a %s: 0x%08x -> 0x%08x\n", sname, sitio, S + A);
                        elf_fracaso(img, tablas, cached);
                        return -1;
                    }
                    parches += 2;
                    break;
                }
                default:
                    printf("elfload: %s: relocacion tipo %u no soportada en %s\n", img->nombre, tipo, shstrtab + sh[tgt].sh_name);
                    elf_fracaso(img, tablas, cached);
                    return -1;
            }
        }
    }

    /* 5. _start */
    for (unsigned i = 0; i < nsym; i++) {
        if (sym[i].st_shndx != SHN_UNDEF && strcmp(strtab + sym[i].st_name, "_start") == 0) {
            start = simval[i];
            break;
        }
    }
    if (start == 0) {
        printf("elfload: %s no tiene _start\n", img->nombre);
        elf_fracaso(img, tablas, cached);
        return -1;
    }

    psram_free(tablas);
    elf_cache_sync(inicio, total, SYNC_INVAL);

    img->bloque = (void *)cached;
    img->tam = total;
    img->entrada = start;

    printf("elfload: %s: %u relocs, %u parches, texto=%uB datos=%uB, _start=0x%08x\n",
           img->nombre, relocs, parches, bytes_texto, bytes_datos, start);
    return 0;
}

/* ---- API publica ---- */
int elf_run(const char *ruta, int argc, char *argv[]) {
    if (!ruta) return -1;

    filehandle *fh = NULL;
    status_t st = fs_open_file(ruta, &fh);
    if (st < 0) {
        printf("elfload: no se abre %s (err %d)\n", ruta, st);
        return -1;
    }
    struct file_stat fst;
    if (fs_stat_file(fh, &fst) < 0 || fst.size == 0 || fst.size > (16u * 1024u * 1024u)) {
        fs_close_file(fh);
        printf("elfload: %s tamano invalido\n", ruta);
        return -1;
    }
    size_t len = (size_t)fst.size;
    uint8_t *buf = (uint8_t *)psram_malloc(len);
    if (!buf) {
        fs_close_file(fh);
        printf("elfload: %s sin %lu bytes en PSRAM\n", ruta, (unsigned long)len);
        return -1;
    }

    size_t off = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 32768) chunk = 32768;
        ssize_t r = fs_read_file(fh, buf + off, (off_t)off, chunk);
        if (r <= 0) break;
        off += (size_t)r;
        thread_yield();
    }
    fs_close_file(fh);
    if (off != len) {
        psram_free(buf);
        printf("elfload: %s leidos %lu de %lu\n", ruta, (unsigned long)off, (unsigned long)len);
        return -1;
    }

    struct elfimg img;
    if (elf_load_image(buf, len, ruta, &img) != 0) {
        psram_free(buf);
        return -1;
    }
    psram_free(buf);

    char *v[ELF_MAX_ARGS + 1];
    int n = 0;
    v[n++] = (char *)ruta;
    for (int i = 0; i < argc && n < ELF_MAX_ARGS; i++) v[n++] = argv[i];
    v[n] = NULL;

    struct elf_thread_ctx *ctx = elf_ctx_alloc(get_current_thread(), &img);
    if (!ctx) {
        printf("elfload: sin contexto para hilo\n");
        psram_free(img.bloque);
        return -1;
    }

    printf("elfload: ejecutando %s (argc=%d)\n", img.nombre, n);
    int (*entry_fn)(int, char **) = (int (*)(int, char **))(uintptr_t)img.entrada;
    int rv = entry_fn(n, v);

    /* si llegamos aqui, main() retorno (no llamo a exit) */
    if (img.bloque) {
        psram_free(img.bloque);
        img.bloque = NULL;
    }
    elf_ctx_free(ctx);

    printf("elfload: %s retorno %d\n", ruta, rv);
    return rv;
}
