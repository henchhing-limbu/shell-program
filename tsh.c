
/* 
 * tsh - A tiny shell program with job control
 * <The line above is not a sufficient documentation.
 *  You will need to write your program documentation.>
 */

#include "tsh_helper.h"
#include <stdio.h>
#include <stdlib.h>
/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);

// TODO:
sigset_t block_signals();
void unblock_signals();

// global variables
int user_interrupt;
// TODO:make the mask global 
// TODO:make a mask function that create masks based on input arguments

/*
 * <Write main's function header documentation. What does main do?>
 * "Each function should be prefaced with a comment describing the purpose
 *  of the function (in a sentence or two), the function's arguments and
 *  return value, any error cases that are relevant to the caller,
 *  any pertinent side effects, and any assumptions that the function makes."
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE_TSH];  // Cmdline for fgets
    bool emit_prompt = true;    // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    Dup2(STDOUT_FILENO, STDERR_FILENO); 
    
    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h':                   // Prints help message
            usage();
            break;
        case 'v':                   // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p':                   // Disables prompt printing
            emit_prompt = false;  
            break;
        default:
            usage();
        }
    }

    // Install the signal handlers
    Signal(SIGINT,  sigint_handler);   // Handles ctrl-c
    Signal(SIGTSTP, sigtstp_handler);  // Handles ctrl-z
    Signal(SIGCHLD, sigchld_handler);  // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    // trace0
    Signal(SIGQUIT, sigquit_handler); 

    // Initialize the job list
    initjobs(job_list);
    
    // initially user interrupt 0
    user_interrupt = 0;

    // Execute the shell's read/eval loop
    while (true)
    {
        // prints the prompt tsh->
        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin))
        {
            app_error("fgets error");
        }

        if (feof(stdin))
        { 
            // End of file (ctrl-d)
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
        }
        
        // Remove the trailing newline
        cmdline[strlen(cmdline)-1] = '\0';
        
        // Evaluate the command line
        eval(cmdline);
        
        fflush(stdout);
    } 
    
    return -1; // control never reaches here
}


/* Handy guide for eval:
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg),
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.
 * Note: each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */

/* 
 * <What does eval do?>
 */
void eval(const char *cmdline) 
{
    parseline_return parse_result;     
    struct cmdline_tokens token;
    // sigset_t ourmask;
    // TODO: remove the line below! It's only here to keep the compiler happy
    // Sigemptyset(&ourmask);

    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY)
    {
        return;
    }
    
    // builtin QUIT command
    if (token.builtin == BUILTIN_QUIT)
    {
        exit(0); 
    }
    
    // builtin jobs command
    else if (token.builtin == BUILTIN_JOBS)
    {
        printf("Jobs command");
    }
    
    // builtin foreground job
    else if (token.builtin == BUILTIN_FG)
    {
        printf("This is foreground job\n");
    }
    
    // built in background job
    else if (token.builtin == BUILTIN_BG) 
    {
        printf("This is background job\n");
    }
    
    else // non builtin commands
    {
        // block SIGCHLD, SIGINT, and SIGTSTP signals
        sigset_t old_mask = block_signals();
        
        // fork the current process
        pid_t pid = Fork();
        
        // process id of child is receievd by parent
        // use that proecss id to add job to the joblist
        if (pid > 0)    
        {       
            if (parse_result == PARSELINE_FG) // Foreground job
            {   
                // adding job to the joblist
                // addjob(job_list, pid, FG, cmdline);
                
                // sigsuspend
                while (!user_interrupt) {
                    Sigsuspend(&old_mask);
                }
                user_interrupt = 0;

                Sigprocmask(SIG_UNBLOCK, &old_mask, NULL);
                
                // ublock signals
                unblock_signals();
            }
            else if (parse_result == PARSELINE_BG) // Background job
            {
                // getting job id
                int job_id = pid2jid(job_list, pid); 
                
                // adding job to the joblist
                addjob(job_list, pid, BG, cmdline);
                
                printf("[%d] (%d) %s\n", job_id+1, pid, cmdline);
                
                Signal(SIGCHLD, sigchld_handler);
            }
        }
        
        else if (pid == 0) {     // child process
            // ublocking the signals
            unblock_signals();
            
            // Restore the signals to default
            Signal(SIGCHLD, SIG_DFL);
            Signal(SIGINT, SIG_DFL);
            Signal(SIGTSTP, SIG_DFL);
            
            // Set new process group id
            Setpgid(0, 0);
            
            // executing the job
            Execve(token.argv[0], token.argv, environ);
        }  
    }
    return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * <What does sigchld_handler do?>
 */
void sigchld_handler(int sig) 
{
    int status;
    pid_t pid;
    while (1)
    {
        pid = waitpid(-1, &status, WUNTRACED | WNOHANG);
        if (pid < 0)
        {
          break;
        }
        if (pid == 0)
            break;
        
        // TODO: only in the case of the job killed or exitede normally
        // delete the job from the job list
        // deletejob(job_list, pid);
    }
    user_interrupt = 1;
    
    return;
}

/* 
 * <What does sigint_handler do?>
 */
void sigint_handler(int sig) 
{
    return;
}

/*
 * <What does sigtstp_handler do?>
 */
void sigtstp_handler(int sig) 
{
    return;
}

/*
 * block SIGCHLD, SIGINT, and SIGTSTP signals
 */
sigset_t block_signals() 
{
    sigset_t mask;
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGCHLD);
    Sigaddset(&mask, SIGINT);
    Sigaddset(&mask, SIGTSTP);
    sigset_t old_mask;
    Sigprocmask(SIG_BLOCK, &mask, &old_mask);
    return old_mask;
}

/*
 * unblock SIGCHLD, SIGINT, and SIGTSTP signals
 */
void unblock_signals() 
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTSTP);
    sigset_t old_mask;
    sigprocmask(SIG_UNBLOCK, &mask, &old_mask);
}