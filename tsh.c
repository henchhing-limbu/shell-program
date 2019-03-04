
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
void handle_background(const char *cmdline, pid_t pid);
void handle_foreground(const char *cmdline, pid_t pid, sigset_t *mask);

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
    
    parse_result = parseline(cmdline, &token);		    // Parse command line

	sigset_t old_mask = block_signals();
    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY)
    {
        return;
    }
     
    if (token.builtin == BUILTIN_QUIT)                      // builtin QUIT command
    {
        exit(0); 
    }
    else if (token.builtin == BUILTIN_JOBS)                 // builtin JOBS command
    {
        listjobs(job_list, STDOUT_FILENO);
        unblock_signals();
    }
    else if (token.builtin == BUILTIN_FG)                   // builtin foreground job
    {
		// TODO: No need to use memcpy here
		// TODO: Make use of pointers
		int s = strlen(token.argv[1]);
		char str_job_id[s];
		memcpy(str_job_id, token.argv[1] + 1, s-1);
		str_job_id[s-1] = '\0';
		int job_id = atoi (str_job_id);
		struct job_t *job = getjobjid(job_list, job_id);
		job->state = FG;
		Kill(- job->pid, SIGCONT);
		
		while (!user_interrupt) {
        	Sigsuspend(&old_mask);
    	}
    	user_interrupt = 0;
    	Sigprocmask(SIG_UNBLOCK, &old_mask, NULL);
		unblock_signals();	
    }
    else if (token.builtin == BUILTIN_BG)                   // built in background job
    {
		int s = strlen(token.argv[1]);
		char str_job_id[s];
		memcpy(str_job_id, token.argv[1] + 1, s-1);
		str_job_id[s-1] = '\0';
		int job_id = atoi (str_job_id);
		struct job_t *job = getjobjid(job_list, job_id);
		job->state = BG;
		Kill(-job->pid, SIGCONT);
		
		sio_puts("[");
		sio_putl(job->jid);
		sio_puts("] (");
		sio_putl(job->pid);
		sio_puts(")  ");
		sio_puts(job->cmdline);
		sio_puts("\n");
		unblock_signals();
	
    } 
    else                                                    // Non builtin commands
    {
		pid_t pid = Fork();                                 // fork the current process
        if (pid > 0)                                        // parent process
        {       
            if (parse_result == PARSELINE_FG)               // Foreground job
            {   
                handle_foreground(cmdline, pid, &old_mask);
            }
            else if (parse_result == PARSELINE_BG)          // Background job
            {
                handle_background(cmdline, pid);
            }
        }
        
        else if (pid == 0) {     			    			// child process
            unblock_signals();
            
			// Restore the signals to default
            Signal(SIGCHLD, SIG_DFL);
            Signal(SIGINT, SIG_DFL);
            Signal(SIGTSTP, SIG_DFL);
            
            Setpgid(0, 0);				    				// set new process id group for child process
            if (token.infile)
			{
				int file_descr = Open(token.infile, O_RDONLY, S_IRWXU);
				Dup2(file_descr,  STDIN_FILENO);
			}
			if (token.outfile) 
			{
				int file_descr = Open(token.outfile, O_WRONLY, S_IRWXU);
				Dup2(file_descr,  STDOUT_FILENO);
			}
            Execve(token.argv[0], token.argv, environ);	// executing the job
        }
		unblock_signals();  
    }
    return;
}

/*
 * handles non built in backgorund job
 */
void handle_background(const char *cmdline, pid_t pid)
{
    addjob(job_list, pid, BG, cmdline);                 
    struct job_t *j = getjobpid(job_list, pid);        
    printf("[%d] (%d) %s\n", j->jid, pid, cmdline);
    Signal(SIGCHLD, sigchld_handler);
}

/*
 * handles non built in foreground job 
 */ 
void handle_foreground(const char *cmdline, pid_t pid, sigset_t *mask)
{
    addjob(job_list, pid, FG, cmdline);
	while (!user_interrupt) {
        Sigsuspend(mask);
    }
    user_interrupt = 0;
    Sigprocmask(SIG_UNBLOCK, mask, NULL);
}

/*****************
 * Signal handlers
 *****************/

/* 
 * handles sigchld handler
 */
void sigchld_handler(int sig) 
{
    block_signals();
    int status;
    pid_t pid, fg_pid;
    while (1)
    {
        pid = waitpid(-1, &status, WUNTRACED | WNOHANG);
        if (pid < 0)    					// No child processes left
          break;
        if (pid == 0)   					// No child processes with state changed
            break;
        
        fg_pid = fgpid(job_list);
		
		if  (WIFEXITED(status))								// child process terminated normally
        {
            deletejob(job_list, pid);							// delete the job from the job list
        }
        else if (WIFSTOPPED(status))							// child process currently stopped
        {
            struct job_t *job = getjobpid(job_list, pid);
            job->state = ST;
			sio_puts("Job [");
			sio_putl(job->jid);
			sio_puts("] (");
			sio_putl(pid);
			sio_puts(") stopped by signal ");
			sio_putl(WSTOPSIG(status));
			sio_puts("\n");
        }
		else if (WIFSIGNALED(status))							// child process terminated due to uncaught signal
		{
			struct job_t *job = getjobpid(job_list, pid);
			sio_puts("Job [");
			sio_putl(job->jid);
			sio_puts("] (");
			sio_putl(pid);
			sio_puts(") terminated by signal ");
			sio_putl(WTERMSIG(status));
			sio_puts("\n");

			deletejob(job_list, pid);
		}
		 if (pid == fg_pid)    
		{
            user_interrupt = 1;
		}
	}
    unblock_signals();
    return;
}

/* 
 * handles sigint signal
 */
void sigint_handler(int sig) 
{
	block_signals();
	Kill(-fgpid(job_list), SIGINT);  
	unblock_signals();  
	return;
}

/*
 * handles sig stop handler
 */
void sigtstp_handler(int sig) 
{
	block_signals();
	Kill(-fgpid(job_list), SIGTSTP);
	unblock_signals();
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
