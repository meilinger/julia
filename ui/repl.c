/*
  repl.c
  system startup, main(), and console interaction
*/

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <signal.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <math.h>
#include <ctype.h>

#ifndef _MSC_VER
#include <unistd.h>
#include <getopt.h>
#else
#include "getopt.h"
#endif

#include "uv.h"
#define WHOLE_ARCHIVE
#include "../src/julia.h"

#ifndef JL_SYSTEM_IMAGE_PATH
#error "JL_SYSTEM_IMAGE_PATH not defined!"
#endif

#ifdef __cplusplus
extern "C" {
#endif

static int lisp_prompt = 0;
static int codecov  = JL_LOG_NONE;
static int malloclog= JL_LOG_NONE;
static int imagepathspecified = 0;

static const char *usage = "julia [options] [program] [args...]\n";
static const char *opts =
    " -v, --version            Display version information\n"
    " -h, --help               Print this message\n"
    " -q, --quiet              Quiet startup without banner\n"
    " -H, --home <dir>         Set location of julia executable\n\n"

    " -e, --eval <expr>        Evaluate <expr>\n"
    " -E, --print <expr>       Evaluate and show <expr>\n"
    " -P, --post-boot <expr>   Evaluate <expr>, but don't disable interactive mode\n"
    " -L, --load <file>        Load <file> immediately on all processors\n"
    " -J, --sysimage <file>    Start up with the given system image file\n"
    " -C --cpu-target <target> Limit usage of cpu features up to <target>\n\n"

    " -p <n>                   Run n local processes\n"
    " --machinefile <file>     Run processes on hosts listed in <file>\n\n"

    " -i                       Force isinteractive() to be true\n"
    " --no-history-file        Don't load or save history\n"
    " -f, --no-startup         Don't load ~/.juliarc.jl\n"
    " -F                       Load ~/.juliarc.jl, then handle remaining inputs\n"
    " --color={yes|no}         Enable or disable color text\n\n"

    " --compile={yes|no|all}   Enable or disable compiler, or request exhaustive compilation\n\n"

    " --code-coverage={none|user|all}, --code-coverage\n"
    "                          Count executions of source lines (omitting setting is equivalent to 'user')\n"
    " --track-allocation={none|user|all}\n"
    "                          Count bytes allocated by each source line\n"
    " --check-bounds={yes|no}  Emit bounds checks always or never (ignoring declarations)\n"
    " -O, --optimize           Run time-intensive code optimizations\n"
    " --int-literals={32|64}   Select integer literal size independent of platform\n"
    " --dump-bitcode={yes|no}  Dump bitcode for the cache image (used with --build)\n"
    " --depwarn={yes|no}       Enable or disable syntax and method deprecation warnings\n";

void parse_opts(int *argcp, char ***argvp)
{
    static char* shortopts = "+H:hJ:C:O";
    static struct option longopts[] = {
        { "home",          required_argument, 0, 'H' },
        { "build",         required_argument, 0, 'b' },
        { "lisp",          no_argument,       &lisp_prompt, 1 },
        { "help",          no_argument,       0, 'h' },
        { "sysimage",      required_argument, 0, 'J' },
        { "code-coverage", optional_argument, 0, 'c' },
        { "cpu-target",    required_argument, 0, 'C' },
        { "track-allocation",required_argument, 0, 'm' },
        { "check-bounds",  required_argument, 0, 300 },
        { "optimize",      no_argument,       0, 'O' },
        { "int-literals",  required_argument, 0, 301 },
        { "dump-bitcode",  required_argument, 0, 302 },
        { "compile",       required_argument, 0, 303 },
        { "depwarn",       required_argument, 0, 304 },
        { 0, 0, 0, 0 }
    };
    int c;
    opterr = 0;
    int skip = 0;
    int lastind = optind;
    while ((c = getopt_long(*argcp,*argvp,shortopts,longopts,0)) != -1) {
        switch (c) {
        case 0:
            break;
        case '?':
            if (optind != lastind) skip++;
            lastind = optind;
            break;
        case 'H':
            jl_compileropts.julia_home = strdup(optarg);
            break;
        case 'b':
            jl_compileropts.build_path = strdup(optarg);
            if (!imagepathspecified)
                jl_compileropts.image_file = NULL;
            break;
        case 'J':
            jl_compileropts.image_file = strdup(optarg);
            imagepathspecified = 1;
            break;
        case 'C':
            jl_compileropts.cpu_target = strdup(optarg);
            break;
        case 'h':
            ios_printf(ios_stdout, "%s%s", usage, opts);
            exit(0);
        case 'O':
            jl_compileropts.opt_level = 1;
            break;
        case 'c':
            if (optarg != NULL) {
                if (!strcmp(optarg,"user"))
                    codecov = JL_LOG_USER;
                else if (!strcmp(optarg,"all"))
                    codecov = JL_LOG_ALL;
                else if (!strcmp(optarg,"none"))
                    codecov = JL_LOG_NONE;
                break;
            }
            else {
                codecov = JL_LOG_USER;
            }
            break;
        case 'm':
            if (optarg != NULL) {
                if (!strcmp(optarg,"user"))
                    malloclog = JL_LOG_USER;
                else if (!strcmp(optarg,"all"))
                    malloclog = JL_LOG_ALL;
                else if (!strcmp(optarg,"none"))
                    malloclog = JL_LOG_NONE;
                break;
            }
        case 300:
            if (!strcmp(optarg,"yes"))
                jl_compileropts.check_bounds = JL_COMPILEROPT_CHECK_BOUNDS_ON;
            else if (!strcmp(optarg,"no"))
                jl_compileropts.check_bounds = JL_COMPILEROPT_CHECK_BOUNDS_OFF;
            break;
        case 301:
            if (!strcmp(optarg,"32"))
                jl_compileropts.int_literals = 32;
            else if (!strcmp(optarg,"64"))
                jl_compileropts.int_literals = 64;
            else {
                ios_printf(ios_stderr, "julia: invalid integer literal size (%s)\n", optarg);
                exit(1);
            }
            break;
        case 302:
            if (!strcmp(optarg,"yes"))
                jl_compileropts.dumpbitcode = JL_COMPILEROPT_DUMPBITCODE_ON;
            else if (!strcmp(optarg,"no"))
                jl_compileropts.dumpbitcode = JL_COMPILEROPT_DUMPBITCODE_OFF;
            break;
        case 303:
            if (!strcmp(optarg,"yes"))
                jl_compileropts.compile_enabled = 1;
            else if (!strcmp(optarg,"no"))
                jl_compileropts.compile_enabled = 0;
            else if (!strcmp(optarg,"all"))
                jl_compileropts.compile_enabled = 2;
            else {
                ios_printf(ios_stderr, "julia: invalid argument to --compile (%s)\n", optarg);
                exit(1);
            }
            break;
        case 304:
            if (!strcmp(optarg,"yes"))
                jl_compileropts.depwarn = 1;
            else if (!strcmp(optarg,"no"))
                jl_compileropts.depwarn = 0;
            else {
                ios_printf(ios_stderr, "julia: invalid argument to --depwarn (%s)\n", optarg);
                exit(1);
            }
            break;
        default:
            ios_printf(ios_stderr, "julia: unhandled option -- %c\n",  c);
            ios_printf(ios_stderr, "This is a bug, please report it.\n");
            exit(1);
        }
    }
    jl_compileropts.code_coverage = codecov;
    jl_compileropts.malloc_log    = malloclog;
    optind -= skip;
    *argvp += optind;
    *argcp -= optind;
}

static int exec_program(char *program)
{
    int err = 0;
 again: ;
    JL_TRY {
        if (err) {
            jl_value_t *errs = jl_stderr_obj();
            jl_value_t *e = jl_exception_in_transit;
            if (errs != NULL) {
                jl_show(jl_stderr_obj(), e);
            }
            else {
                jl_printf(JL_STDERR, "error during bootstrap: ");
                jl_static_show(JL_STDERR, e);
            }
            jl_printf(JL_STDERR, "\n");
            JL_EH_POP();
            return 1;
        }
        jl_load(program);
    }
    JL_CATCH {
        err = 1;
        goto again;
    }
    return 0;
}

void jl_lisp_prompt();

#ifndef _WIN32
int jl_repl_raise_sigtstp(void)
{
    return raise(SIGTSTP);
}
#endif

#ifdef JL_GF_PROFILE
static void print_profile(void)
{
    size_t i;
    void **table = jl_base_module->bindings.table;
    for(i=1; i < jl_base_module->bindings.size; i+=2) {
        if (table[i] != HT_NOTFOUND) {
            jl_binding_t *b = (jl_binding_t*)table[i];
            if (b->value != NULL && jl_is_function(b->value) &&
                jl_is_gf(b->value)) {
                ios_printf(ios_stdout, "%d\t%s\n",
                           jl_gf_mtable(b->value)->ncalls,
                           jl_gf_name(b->value)->name);
            }
        }
    }
}
#endif

static int true_main(int argc, char *argv[])
{
    if (jl_base_module != NULL) {
        jl_array_t *args = (jl_array_t*)jl_get_global(jl_base_module, jl_symbol("ARGS"));
        if (args == NULL) {
            args = jl_alloc_cell_1d(0);
            jl_set_const(jl_base_module, jl_symbol("ARGS"), (jl_value_t*)args);
        }
        assert(jl_array_len(args) == 0);
        jl_array_grow_end(args, argc);
        int i;
        for (i=0; i < argc; i++) {
            jl_value_t *s = (jl_value_t*)jl_cstr_to_string(argv[i]);
            s->type = (jl_value_t*)jl_utf8_string_type;
            jl_arrayset(args, s, i);
        }
    }

    jl_function_t *start_client = jl_base_module ?
        (jl_function_t*)jl_get_global(jl_base_module, jl_symbol("_start")) : NULL;

    if (start_client) {
        jl_apply(start_client, NULL, 0);
        return 0;
    }

    // run program if specified, otherwise enter REPL
    if (argc > 0) {
        if (strcmp(argv[0], "-")) {
            return exec_program(argv[0]);
        }
    }

    int iserr = 0;

 again:
    ;
    char *line = NULL;
    while (!ios_eof(ios_stdin)) {
        JL_TRY {
            if (iserr) {
                //jl_show(jl_exception_in_transit);# What if the error was in show?
                jl_printf(JL_STDERR, "\nerror in printing:\n");
                jl_static_show(JL_STDOUT, jl_exception_in_transit);
                JL_PUTS("\n",JL_STDOUT);
                iserr = 0;
            } else {
                ios_puts("\njulia> ", ios_stdout);
                ios_flush(ios_stdout);
                line = ios_readline(ios_stdin);
                jl_value_t *val = jl_eval_string(line);
                if (jl_exception_occurred()) {
                    JL_PUTS("error during run:\n", JL_STDERR);
                    jl_static_show(JL_STDOUT, jl_exception_in_transit);
                    jl_exception_clear();
                } else if (val) {
                    jl_static_show(JL_STDOUT, val);
                }
                JL_PUTS("\n", JL_STDOUT);
                free(line);
                line = NULL;
            }
            uv_run(jl_global_event_loop(),UV_RUN_NOWAIT);
        }
        JL_CATCH {
            if (line) {
                free(line);
                line = NULL;
            }
            iserr = 1;
            goto again;
        }
    }
    return iserr;
}

#ifndef _OS_WINDOWS_
int main(int argc, char *argv[])
{
#else
int wmain(int argc, wchar_t *argv[], wchar_t *envp[])
{
    int i;
    for (i=0; i<argc; i++) { // write the command line to UTF8
        wchar_t *warg = argv[i];
        size_t wlen = wcslen(warg)+1;
        size_t len = WideCharToMultiByte(CP_UTF8, 0, warg, wlen, NULL, 0, NULL, NULL);
        if (!len) return 1;
        char *arg = (char*)alloca(len);
        if (!WideCharToMultiByte(CP_UTF8, 0, warg, wlen, arg, len, NULL, NULL)) return 1;
        argv[i] = (wchar_t*)arg;
    }
#endif
    libsupport_init();
    parse_opts(&argc, (char***)&argv);
    if (lisp_prompt) {
        jl_lisp_prompt();
        return 0;
    }
    julia_init(imagepathspecified ? JL_IMAGE_CWD : JL_IMAGE_JULIA_HOME);
    int ret = true_main(argc, (char**)argv);
    jl_atexit_hook();
    return ret;
}

#ifdef __cplusplus
}
#endif
