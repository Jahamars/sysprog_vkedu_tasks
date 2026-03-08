#include "parser.h"

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char **
build_argv(const struct command *cmd)
{
    char **argv = malloc(sizeof(char *) * (cmd->arg_count + 2));
    argv[0] = cmd->exe;
    for (uint32_t i = 0; i < cmd->arg_count; i++)
        argv[i + 1] = cmd->args[i];
    argv[cmd->arg_count + 1] = NULL;
    return argv;
}

static const struct expr *
find_seg_end(const struct expr *e)
{
    while (e != NULL && e->type != EXPR_TYPE_AND && e->type != EXPR_TYPE_OR)
        e = e->next;
    return e;
}


static int
exec_segment(const struct expr *start, const struct expr *end,
             const char *out_file, int out_flags,
             int is_background,
             int *exit_in_shell, int *exit_code_out)
{
    int ncmds = 0;
    for (const struct expr *e = start; e != end; e = e->next)
        if (e->type == EXPR_TYPE_COMMAND)
            ncmds++;

    if (ncmds == 0)
        return 0;

    const struct command **cmds = malloc(sizeof(*cmds) * ncmds);
    int ci = 0;
    for (const struct expr *e = start; e != end; e = e->next)
        if (e->type == EXPR_TYPE_COMMAND)
            cmds[ci++] = &e->cmd;

    /* Builtins only as single command with no redirect */
    if (ncmds == 1 && out_file == NULL) {
        const struct command *cmd = cmds[0];
        if (strcmp(cmd->exe, "cd") == 0) {
            free(cmds);
            if (cmd->arg_count == 0) {
                const char *home = getenv("HOME");
                if (home) chdir(home);
            } else {
                chdir(cmd->args[0]);
            }
            return 0;
        }
        if (strcmp(cmd->exe, "exit") == 0) {
            free(cmds);
            int code = cmd->arg_count > 0 ? atoi(cmd->args[0]) : 0;
            *exit_in_shell = 1;
            *exit_code_out = code;
            return code;
        }
    }

    pid_t *pids = malloc(sizeof(pid_t) * ncmds);

    
    int prev_read = -1; 

    for (int i = 0; i < ncmds; i++) {
        const struct command *cmd = cmds[i];
        int cur_pipe[2] = {-1, -1};
        int is_last = (i == ncmds - 1);

        if (!is_last)
            pipe(cur_pipe);

        pid_t pid = fork();
        if (pid == 0) {
            if (prev_read >= 0) {
                dup2(prev_read, STDIN_FILENO);
                close(prev_read);
            }
            if (!is_last) {
                dup2(cur_pipe[1], STDOUT_FILENO);
                close(cur_pipe[0]);
                close(cur_pipe[1]);
            } else if (out_file != NULL) {
                int fd = open(out_file, out_flags, 0644);
                if (fd >= 0) {
                    dup2(fd, STDOUT_FILENO);
                    close(fd);
                }
            }

            if (strcmp(cmd->exe, "exit") == 0) {
                int code = cmd->arg_count > 0 ? atoi(cmd->args[0]) : 0;
                _exit(code);
            }
            if (strcmp(cmd->exe, "cd") == 0) {
                if (cmd->arg_count > 0)
                    chdir(cmd->args[0]);
                _exit(0);
            }

            char **argv = build_argv(cmd);
            execvp(cmd->exe, argv);
            free(argv);
            _exit(127);
        }

        if (prev_read >= 0)
            close(prev_read);
        if (!is_last) {
            close(cur_pipe[1]);      
            prev_read = cur_pipe[0]; 
        }

        pids[i] = pid;
    }

    int last_status = 0;
    if (!is_background) {
        for (int i = 0; i < ncmds; i++) {
            int status;
            waitpid(pids[i], &status, 0);
            if (i == ncmds - 1)
                last_status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
    }

    free(pids);
    free(cmds);
    return last_status;
}

static int
execute_command_line(const struct command_line *line)
{
    assert(line != NULL);

    const char *out_file = NULL;
    int out_flags = 0;
    if (line->out_type == OUTPUT_TYPE_FILE_NEW) {
        out_file = line->out_file;
        out_flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (line->out_type == OUTPUT_TYPE_FILE_APPEND) {
        out_file = line->out_file;
        out_flags = O_WRONLY | O_CREAT | O_APPEND;
    }

    const struct expr *e = line->head;
    int last_code = 0;
    int exit_in_shell = 0;
    int exit_code = 0;

    while (e != NULL) {
        const struct expr *seg_end = find_seg_end(e);

        const char *seg_file = (seg_end == NULL) ? out_file : NULL;
        int seg_flags = (seg_end == NULL) ? out_flags : 0;

        last_code = exec_segment(e, seg_end, seg_file, seg_flags,
                                 line->is_background,
                                 &exit_in_shell, &exit_code);

        if (exit_in_shell)
            break;
        if (seg_end == NULL)
            break;

        enum expr_type op = seg_end->type;
        e = seg_end->next;

        while (e != NULL) {
            if (op == EXPR_TYPE_AND && last_code != 0) {
                const struct expr *nop = find_seg_end(e);
                if (nop == NULL) { e = NULL; break; }
                op = nop->type;
                e = nop->next;
            } else if (op == EXPR_TYPE_OR && last_code == 0) {
                const struct expr *nop = find_seg_end(e);
                if (nop == NULL) { e = NULL; break; }
                op = nop->type;
                e = nop->next;
            } else {
                break;
            }
        }

        if (e == NULL)
            break;
    }

    if (exit_in_shell)
        exit(exit_code);

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

    return last_code;
}

int
main(void)
{
    const size_t buf_size = 1024;
    char buf[buf_size];
    int rc;
    int last_code = 0;
    struct parser *p = parser_new();

    while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
        parser_feed(p, buf, (uint32_t)rc);
        struct command_line *line = NULL;
        while (1) {
            enum parser_error err = parser_pop_next(p, &line);
            if (err == PARSER_ERR_NONE && line == NULL)
                break;
            if (err != PARSER_ERR_NONE)
                continue;
            last_code = execute_command_line(line);
            command_line_delete(line);
        }
    }
    {
        struct command_line *line = NULL;
        while (1) {
            enum parser_error err = parser_pop_next(p, &line);
            if (err == PARSER_ERR_NONE && line == NULL)
                break;
            if (err != PARSER_ERR_NONE)
                break;
            last_code = execute_command_line(line);
            command_line_delete(line);
        }
    }
    parser_delete(p);
    return last_code;
}
