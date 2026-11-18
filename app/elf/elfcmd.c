/*
 * Comando 'elf' para LK: carga y ejecuta un ELF ET_REL RV32 desde el FS.
 * Inspirado en xsh_elf.c de Xinu.
 *   elf /sd/appslkx/app.elf [args...]
 */
#include <app.h>
#include <lk/console_cmd.h>
#include <lib/elfload.h>
#include <lib/elf_exports.h>
#include <lib/fs.h>
#include <kernel/thread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

struct elf_run_ctx {
    char ruta[128];
    int app_argc;
    char *app_argv[32];
    char argbuf[512];
};

static int elf_thread_entry(void *arg) {
    struct elf_run_ctx *ctx = (struct elf_run_ctx *)arg;
    int rc = elf_run(ctx->ruta, ctx->app_argc, ctx->app_argv);
    return rc;
}

static int cmd_elf(int argc, const console_cmd_args *argv) {
    if (argc < 2 || strcmp(argv[1].str, "--help") == 0) {
        printf("uso: elf <ruta.elf> [args...]\n");
        printf("  Carga un ELF ET_REL RV32 (ld -r) desde el FS, enlaza contra\n");
        printf("  los exports del kernel y ejecuta _start(argc,argv).\n");
        printf("  Ej: elf /sd/appslkx/hello.elf\n");
        return 0;
    }

    struct elf_run_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        printf("elf: sin memoria\n");
        return -1;
    }
    strlcpy(ctx->ruta, argv[1].str, sizeof(ctx->ruta));

    /* copiar args a buffer propio */
    size_t off = 0;
    for (int i = 2; i < argc && ctx->app_argc < 31; i++) {
        const char *s = argv[i].str;
        size_t len = strlen(s) + 1;
        if (off + len > sizeof(ctx->argbuf)) break;
        memcpy(ctx->argbuf + off, s, len);
        ctx->app_argv[ctx->app_argc++] = ctx->argbuf + off;
        off += len;
    }
    ctx->app_argv[ctx->app_argc] = NULL;

    printf("elf: cargando %s\n", ctx->ruta);
    thread_t *t = thread_create("elf_app", elf_thread_entry, ctx, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE * 2);
    if (!t) {
        printf("elf: no se pudo crear hilo\n");
        free(ctx);
        return -1;
    }
    thread_resume(t);

    int retcode = 0;
    thread_join(t, &retcode, INFINITE_TIME);
    free(ctx);

    if (retcode < 0) {
        printf("elf: fallo al cargar/ejecutar %s (rc=%d)\n", argv[1].str, retcode);
        return -1;
    }
    printf("elf: %s finalizo (rc=%d)\n", argv[1].str, retcode);
    return 0;
}

static int cmd_elfexports(int argc, const console_cmd_args *argv) {
    (void)argc; (void)argv;
    unsigned int n = elf_export_count();
    printf("exports: %u simbolos\n", n);
    for (unsigned int i = 0; i < n; i++) {
        const char *name = elf_export_name(i);
        if (name) printf("  %s\n", name);
    }
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("elf", "cargar y ejecutar ELF ET_REL desde SD/FS", &cmd_elf)
STATIC_COMMAND("elfexports", "listar simbolos exportados a apps ELF", &cmd_elfexports)
STATIC_COMMAND_END(elfcmd);
