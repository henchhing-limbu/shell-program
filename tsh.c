
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

void handle_background(const char *cmdline, pid_t pid);
void handle_foreground(const char *cmdline, pid_t pid);
void state_bg_jobs(struct job_t *job);
void state_change_info(int jid, int pid, int signum, char change);
int get_job_id(struct cmdline_tokens token);

// global variables
int user_interrupt;
sigset_t mask, old_mask;
int def_in_desc, def_out_desc;

/*
 * main -
 * 		-> sets the signal handlers
 *		-> creates shell prompt "tsh->" and waits for user input in a loop
 *		-> parses the command line to display needed info
 *		-> calls eval to handle the input command
 *
 * argc		: number of arguments from command line
 * argv		: commandline arguments
 *
 * return 	: exit code
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

	// initialize user_interrupt to 0
	user_interrupt = 0;
	
	// initializing def_in_desc and def_out_desc
	// def_in_desc = dup(STDIN_FILENO);
	// def_out_desc = dup(STDOUT_FILENO);

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

/* 
 * eval -
 * 	-> parse the command line
 * 	-> executes the builtin commands
 *	-> forks and runs the job in context of child process
 *	-> calls helper functions to handle background and foreground jobs
 *	-> calls helper functions to display background jobs
 *
 * cmdline : command entered in the shell
 */
void eval(const char *cmdline) 
{
	// create a mask
	Sigemptyset(&mask);
	Sigaddset(&mask, SIGCHLD);
	Sigaddset(&mask, SIGINT);
	Sigaddset(&mask, SIGTSTP);
	Sigprocmask(SIG_BLOCK, &mask, &old_mask);

	parseline_return parse_result;    
	struct cmdline_tokens token;
	// Parse command line
	parse_result = parseline(cmdline, &token);		  
	
	if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY)
        	return;
	
	int in_desc = STDIN_FILENO;
	int out_desc = STDOUT_FILENO; 
	// I/O redirection for builtin commands
	if (token.builtin != BUILTIN_NONE)	
	{
		// input redirection
 		if (token.infile)
		{
			in_desc = Open(token.infile, O_RDONLY, S_IRWXU);
			def_in_desc = dup(STDIN_FILENO);
			Dup2(in_desc, STDIN_FILENO);
		}
		// output redirection
		if (token.outfile)
		{
			out_desc = Open(token.outfile, O_WRONLY | O_CREAT, S_IRWXU);
			def_out_desc = dup(STDOUT_FILENO);
			Dup2(out_desc, STDOUT_FILENO);
		}
	}

	// builtin QUIT command
	if (token.builtin == BUILTIN_QUIT)                      
        	exit(0); 
	
	// builtin JOBS command
	else if (token.builtin == BUILTIN_JOBS)                
	{
		listjobs(job_list, STDOUT_FILENO);
		Sigprocmask(SIG_UNBLOCK, &mask, NULL);
	}
	// builtin foreground job
	else if (token.builtin == BUILTIN_FG)                   
	{
		// parse the argument to get job id
		int job_id = get_job_id(token);

		// change the job state to FG
		// forward SIGCONT signal to every associated FG child process
		struct job_t *job = getjobjid(job_list, job_id);
		job->state = FG;
		Kill(- job->pid, SIGCONT);
		
		// suspend until child process are done
		while (!user_interrupt) {
        	Sigsuspend(&old_mask);
    	}
    	user_interrupt = 0;
    	Sigprocmask(SIG_UNBLOCK, &old_mask, NULL);		
		
	Sigprocmask(SIG_UNBLOCK, &mask, NULL);	
	}
	// built in background job
	else if (token.builtin == BUILTIN_BG)                   
	{
		// parse the argument to get job id
		int job_id = get_job_id(token); 
		
		// change the job state to BG
		// forward SIGCONT signal to every associated BG child process
		struct job_t *job = getjobjid(job_list, job_id);
		job->state = BG;
		Kill(-job->pid, SIGCONT);
		
		// print background job info	
		state_bg_jobs(job);	
	
		Sigprocmask(SIG_UNBLOCK, &mask, NULL);	
	}
    
	// Non builtin commands
	else                                                    
	{
		pid_t pid = Fork();
		// parent process
        	if (pid > 0)                                        
        	{      
            		if (parse_result == PARSELINE_FG)               
            		{   
                		// handles and executes foreground job 
				handle_foreground(cmdline, pid);
            		}
            		else if (parse_result == PARSELINE_BG)   
            		{
				// handles and executes background job
                		handle_background(cmdline, pid);
            		}
        	}
        	// child process
        	else if (pid == 0) 
		{     			    		
            		Sigprocmask(SIG_UNBLOCK, &mask, NULL);

			// Restore the signals to default
            		Signal(SIGCHLD, SIG_DFL);
            		Signal(SIGINT, SIG_DFL);
            		Signal(SIGTSTP, SIG_DFL);

            		// set new process id group for child process
            		Setpgid(0, 0);
			// input redirection				    				
            		if (token.infile)
			{
				in_desc = Open(token.infile, O_RDONLY, S_IRWXU);
				Dup2(in_desc,  STDIN_FILENO);
			}
			// output redirection
			if (token.outfile) 
			{
				out_desc = Open(token.outfile, O_WRONLY | O_CREAT, S_IRWXU);
				Dup2(out_desc,  STDOUT_FILENO);
			}
            		Execve(token.argv[0], token.argv, environ);
        	}
		Sigprocmask(SIG_UNBLOCK, &mask, NULL);
    	}
	
	if (token.builtin != BUILTIN_NONE)
	{
		// input redirection				    				
		if (token.infile)
		{
			Dup2(def_in_desc, STDIN_FILENO);
			// Close the infile
			Close(in_desc);
		}
		// output redirection
		if (token.outfile) 
		{
			Dup2(def_out_desc, STDOUT_FILENO);
			// close the outfile
			Close(out_desc);
		}
	}
	return;
}

/*
 * handle_background - 
 * 		-> adds job to the job list
 * cmdline : command line arguments  
 * pid     : process id of the job
 *          
 */
void handle_background(const char *cmdline, pid_t pid)
{
	addjob(job_list, pid, BG, cmdline);                 
	struct job_t *j = getjobpid(job_list, pid);        
	state_bg_jobs(j);	
}

/*
 * handle_foreground -
 * 		-> adds job to the job list
 * 		-> suspend process until signal not in mask is delivered
 * cmdline : command line arguments
 * pid     : process id of the job
 * mask    : signal mask used by sigsuspend
 */ 
void handle_foreground(const char *cmdline, pid_t pid)
{
	addjob(job_list, pid, FG, cmdline);
	while (!user_interrupt) {
        	Sigsuspend(&old_mask);
    	}
    	user_interrupt = 0;
    	Sigprocmask(SIG_UNBLOCK, &old_mask, NULL);
}

/*
 * state_change_info -
 * 		-> prints info on job that changed state
 * jid 		: job id
 * pid  	: process id
 * signum	: signal that changed the job state
 * change	: char that denotes type of change in job state
 */
void state_change_info(int jid, int pid, int signum, char change)
{
	switch (change)
	{
		// job stopped
		case 'S':
			Sio_puts("Job [");
			Sio_putl(jid);
			Sio_puts("] (");
			Sio_putl(pid);
			Sio_puts(") stopped by signal ");
			Sio_putl(signum);
			Sio_puts("\n");
			break;
		// job terminated
		case 'T':
			Sio_puts("Job [");
			Sio_putl(jid);
			Sio_puts("] (");
			Sio_putl(pid);
			Sio_puts(") terminated by signal ");
			Sio_putl(signum);
			Sio_puts("\n");
			break;
	}
}

/*
 * state_bg_jobs - display the background job info
 * job		: pointer to the associated background job in job list
 */
void state_bg_jobs(struct job_t *job)
{
	// print the background jobs info
	sio_puts("[");
	sio_putl(job->jid);
	sio_puts("] (");
	sio_putl(job->pid);
	sio_puts(")  ");
	sio_puts(job->cmdline);
	sio_puts("\n");
}

/*
 *	get_job_id - parse the token to get job id
 *	token	: struct that contains commandline tokens
 *	return	: job id 
 */
int get_job_id(struct cmdline_tokens token)
{
	int s = strlen(token.argv[1]);
	char str_job_id[s];
	memcpy(str_job_id, token.argv[1] + 1, s-1);
	str_job_id[s-1] = '\0';
	int job_id = atoi (str_job_id);
	return job_id;
}
/*****************
 * Signal handlers
 *****************/

/* 
 * sig: signal from child process that invoked sigchild handler
 * 
 * sigchld_handler - 
 * Invoked when the child process state changes
 * If child process terminated normally:
 * 		->	delete job from the job list
 * If child process stopped:
 *		-> change job state to ST
 *		-> print info on stopped job
 * If child process terminated due to uncaught signal:
 *		-> delete job from job list
 *		-> print info on terminated job
 * If foreground child process;
 *		-> assign 1 to user_interrupt
 * 
 */ 
void sigchld_handler(int sig) 
{
	Sigprocmask(SIG_BLOCK, &mask, NULL);

    	int status;
    	pid_t pid, fg_pid;

    	while (1)
    	{
        	pid = waitpid(-1, &status, WUNTRACED | WNOHANG);
		// No child processes left
        	if (pid < 0)    					
          		break;
		// No child processes with state changed
        	if (pid == 0)   					
            		break;
        
        	fg_pid = fgpid(job_list);
		// child process terminated normally
		if  (WIFEXITED(status))								
            		deletejob(job_list, pid);
		// child process currently stopped
	        else if (WIFSTOPPED(status))							
       		{
			// change job state in job list to stop
            		struct job_t *job = getjobpid(job_list, pid);
            		job->state = ST;
		
			// print stopped job info	
			state_change_info(job->jid, pid, WSTOPSIG(status), 'S');
        	}
		// child process terminated due to uncaught signal
		else if (WIFSIGNALED(status))						
		{
			struct job_t *job = getjobpid(job_list, pid);
			
			// print the info on terminated process	
			state_change_info(job->jid, pid, WTERMSIG(status), 'T');
			
			// deleting the terminated job from job list
			deletejob(job_list, pid);
		}
		
		// foreground child process
		if (pid == fg_pid)    
            		user_interrupt = 1;
	}
    	Sigprocmask(SIG_UNBLOCK, &mask, NULL);
	return;
}

/* 
 * sig: signal from child process that invoked the sigint handler
 * sigint_handler - forwards SIGINT to every process in foreground
 * process group 
 */
void sigint_handler(int sig) 
{
	Sigprocmask(SIG_BLOCK, &mask, NULL);
	Kill(-fgpid(job_list), SIGINT);  
	Sigprocmask(SIG_UNBLOCK, &mask, NULL);
	return;
}

/*
 * sig: signal from child process that invoked the sigtstp handler
 * sigtstp_handler - forwards SIGTSTP to every process in foreground
 * process group 
 */
void sigtstp_handler(int sig) 
{
	Sigprocmask(SIG_BLOCK, &mask, NULL);
	Kill(-fgpid(job_list), SIGTSTP);
	Sigprocmask(SIG_UNBLOCK, &mask, NULL);
	return;
}
