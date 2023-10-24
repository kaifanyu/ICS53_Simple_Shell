#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>
#include <fcntl.h>

// Array size values
#define MAXLINE 80
#define MAXARGS 80
#define MAXJOB 5

// signal handlers
pid_t fg_pid = 0;

// File Redirect values
static int file_input;
static int file_output;
static mode_t mode = S_IRWXU | S_IRWXG | S_IRWXO;
typedef struct
{
    int id;
    pid_t pid;
    int state; // 1 = fg, 2 = bg, 3 = stopped
    char *cmd;
} job;

job jobArray[MAXJOB];

// whenever a process is deleted, clean up job array
void deleteJob(pid_t pid)
{
    if (pid != 0)
    {
        for (size_t i = 0; i < MAXJOB; i++)
        {
            if (jobArray[i].pid == pid)
            {
                jobArray[i].id = i;
                jobArray[i].pid = 0;
                jobArray[i].state = 0;
                jobArray[i].cmd = strdup("NULL");
                break;
            }
        }
    }
}

// initialize job array
void fillJobArr(job *arr)
{
    for (size_t i = 0; i < MAXJOB; i++)
    {
        arr[i].id = i;
        arr[i].pid = 0;
        arr[i].state = 0;
        arr[i].cmd = strdup("NULL");
    }
}
// assign jobs to processses
void assignJob(job *jobArr, pid_t pid, int state, char *cmd)
{
    for (size_t i = 0; i < MAXJOB; i++)
    {
        if (jobArr[i].state == 0)
        {
            jobArr[i].id = i;
            jobArr[i].pid = pid;
            jobArr[i].state = state + 1;
            jobArr[i].cmd = strdup(cmd);
            break;
        }
    }
}
int isFull(job *jobArr)
{
    for (size_t i = 0; i < MAXJOB; i++)
    {
        if (jobArr[i].pid == 0 && jobArr[i].state == 0)
        {
            return 0;
        }
    }
    return 1;
}

void printJobs()

{
    int n = sizeof(jobArray) / sizeof(jobArray[0]); // Size of the array

    for (int i = 0; i < n; i++)
    {
        if (jobArray[i].state > 0)
        { // Corrected the comparison
            char *temp;
            switch (jobArray[i].state)
            {
            case 2:
                temp = "Running";
                break;
            case 3:
                temp = "Stopped";
                break;
            case 1:
                temp = "Foreground";
                break;
            default:
                temp = "N/A";
            }
            printf("[%d] (%d) %s %s\n", jobArray[i].id + 1, jobArray[i].pid, temp, jobArray[i].cmd);
            // Print more fields if needed
        }
    }
}
// handles ctrl-C for fg process
// NOTE: we use SIGQUIT instead if SIGINT because children ignore SIGINT
void sigint_handler(int sig)
{
    if (fg_pid != 0)
    {
        kill(fg_pid, SIGINT);
        deleteJob(fg_pid);
        // printf("child %d terminated\n", fg_pid);
    }
}
// handles ctrl+z for fg process
// NOTE: we use SIGSTOP instead if SIGTSTP because children ignore SIGTSTP
void sigstp_handler(int sig)
{
    if (fg_pid != 0)
    {
        kill(fg_pid, SIGTSTP);
        // printf("child %d stopped\n", fg_pid);
        // turn process into stopped state
        for (size_t i = 0; i < MAXJOB; i++)
        {
            if (jobArray[i].pid == fg_pid)
            {
                jobArray[i].id = i;
                jobArray[i].pid = fg_pid;
                jobArray[i].state = 3;
                fg_pid = 0;
                break;
            }
        }
    }
}

// kills child if child doesn't die
void sigchld_handler(int sig)
{
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    {
        // printf("child %d terminated\n", pid);
        deleteJob(pid);
    }
}

// report error
void unix_error(const char *msg)
{
    perror(msg);
    exit(EXIT_FAILURE);
}

pid_t findPid(char *argv[MAXARGS])
{
    pid_t pid = 0;
    if (argv[1] == NULL)
    {
        return pid;
    }

    if (argv[1][0] == '%')
    {
        int job_id = atoi(&argv[1][1]) - 1;
        for (size_t i = 0; i < MAXJOB; i++)
        {
            if (jobArray[i].id == job_id)
            {
                pid = jobArray[i].pid;
                break;
            }
        }
    }
    else
    {
        pid = atoi(argv[1]);
    }
    return pid;
}

// Tokenizer function
int parseline(char *cmdline, char *argv[MAXARGS])
{
    char *str = cmdline;
    char *token;
    int argc = 0;
    int bg = 0;

    // Parse commandline into argv
    token = strtok(cmdline, " \t");
    while (token != NULL && argc < MAXARGS - 1)
    {
        argv[argc++] = token;
        token = strtok(NULL, " \t");
    }
    argv[argc] = NULL; // Set the last element to NULL as required by exec functions

    // make all newlines end with null char
    for (int i = 0; i < argc; i++)
    {
        int len = strlen(argv[i]);
        if (len > 0 && argv[i][len - 1] == '\n')
        {
            argv[i][len - 1] = '\0';
        }
    }

    // check if last argv contains &
    if (argv[argc - 1][0] == '&')
    {
        argv[argc - 1][0] = '\0';
        return 1;
    }
    return 0;
}

// check if cmd is builtin
int builtin_command(char *argv[MAXARGS])
{
    if (strcmp(argv[0], "quit") == 0)
    {
        for (size_t i = 0; i < MAXJOB; i++)
        {
            pid_t pid = jobArray[i].pid;
            if (pid != 0)
            {
                kill(pid, SIGINT);
                deleteJob(pid);
            }
        }

        exit(0);
        return 1;
    }
    else if (strcmp(argv[0], "pwd") == 0)
    {
        char cwd[1028];
        getcwd(cwd, sizeof(cwd));
        printf("%s\n", cwd);
        return 1;
    }
    else if (strcmp(argv[0], "cd") == 0)
    {
        if (argv[1] == NULL)
        {
            fprintf(stderr, "cd: syntax error\n");
        }
        else
        {
            if (chdir(argv[1]) != 0)
            {
                perror("chdir() error");
            }
        }
        return 1;
    }
    else if (strcmp(argv[0], "jobs") == 0)
    {
        printJobs();
        return 1;
    }
    // Swapping bg tasks to fg
    else if (strcmp(argv[0], "fg") == 0)
    { 
        pid_t pid = findPid(argv);
        if (pid != 0)
        {
            kill(pid, SIGCONT);

            fg_pid = pid;
            int status;
            for (size_t i = 0; i < MAXJOB; i++)
            {
                if (jobArray[i].pid == pid)
                {
                    jobArray[i].state = 1;
                }
            }

            if (waitpid(pid, &status, (WUNTRACED)) < 0)
            {
                unix_error("waitfg: waitpid error");
            }
            for (size_t i = 0; i < MAXJOB; i++)
            {
                // delete jobs that have finished running
                if (jobArray[i].pid == pid && jobArray[i].state == 1)
                {
                    deleteJob(pid);
                    break;
                }
            }
        }
        return 1;
    }
    // Swapping reactivating bg tasks
    else if (strcmp(argv[0], "bg") == 0)
    {
        pid_t pid = findPid(argv);
        if (pid != 0)
        {
            kill(pid, SIGCONT);
            for (size_t i = 0; i < MAXJOB; i++)
            {
                if (jobArray[i].pid == pid)
                {
                    jobArray[i].state = 2;
                }
            }
        }
        return 1;
    }
    // kill command to any job
    else if (strcmp(argv[0], "kill") == 0)
    {
        pid_t pid = findPid(argv);
        if (pid != 0)
        {
            kill(pid, SIGKILL);
            deleteJob(pid);
        }
        return 1;
    }
    return 0;
}
int findInput(char **argv)
{
    int temp = 0;
    int i = 0;
    while (argv[i] != NULL)
    {
        if (argv[i] != NULL && strcmp(argv[i], "<") == 0)
        {
            temp = 1;
        }
        else if (argv[i] != NULL && strcmp(argv[i], ">") == 0)
        {
            if (temp == 1)
            {
                temp = 3;
            }
            else
            {
                temp = 2;
            }
        }
        if (argv[i] != NULL && strcmp(argv[i], ">>") == 0)
        {
            temp = 4;
        }
        i++;
    }
    return temp;
}

void file_out(char **argv)
{
    char *filename = NULL;
    for (size_t i = 1; argv[i] != NULL; i++)
    {
        if (strcmp(argv[i], ">") == 0)
        {
            filename = argv[i + 1];
            argv[i] = NULL;
        }
    }

    // set file_IO to current IO
    file_input = dup(STDIN_FILENO);
    file_output = dup(STDOUT_FILENO);

    // we are just using hi as input, open the file at argv[2] = jobs > output.txt
    // set openfile = id of newFile
    int openFile = open(filename, O_CREAT | O_WRONLY | O_TRUNC, mode);
    // dup 2 to change output stream to that file
    dup2(openFile, STDOUT_FILENO);
    // close the file after we are done using it
    close(openFile);
    filename = NULL;
}
void append(char **argv)
{
    char *filename = NULL;
    for (size_t i = 1; argv[i] != NULL; i++)
    {
        if (strcmp(argv[i], ">>") == 0)
        {
            filename = argv[i + 1];
            argv[i] = NULL;
        }
    }

    file_input = dup(STDIN_FILENO);
    file_output = dup(STDOUT_FILENO);

    int appendFile = open(filename, O_APPEND | O_CREAT | O_WRONLY, mode);
    dup2(appendFile, STDOUT_FILENO);
    close(appendFile);
    filename = NULL;
}

void file_in(char **argv)
{
    char *filename;
    for (size_t i = 1; argv[i] != NULL; i++)
    {
        if (strcmp(argv[i], "<") == 0)
        {
            filename = argv[i + 1];
            argv[i] = NULL;
        }
    }

    FILE *input_file = fopen(filename, "r");
    if (input_file == NULL)
    {
        fprintf(stderr, "Error opening the file.\n");
        return;
    }
    // allocate variable/memory on the heap
    char *buffer = malloc(MAXLINE * sizeof(char));
    fgets(buffer, MAXLINE, input_file);
    fclose(input_file);
    argv[1] = buffer;
    filename = NULL;
    return;
}

void eval(char *cmdline)
{
    char *argv[MAXARGS]; // argv for execve()?
    int bg;              // determine job for fg or bg. 1 is bg and 0 is fg
    pid_t pid;           // process id
    // parse the cmd to argv[]
    char copy_cmd[128];

    strcpy(copy_cmd, cmdline);
    copy_cmd[strcspn(copy_cmd, "\n")] = 0;
    bg = parseline(cmdline, argv);

    // prompt > jobs < output.txt
    if (findInput(argv) == 1) // input
    {
        file_in(argv);
    }

    // prompt> jobs > output.txt
    else if (findInput(argv) == 2) // output
    {
        file_out(argv);
    }
    else if (findInput(argv) == 3)
    {
        // Manipulating in place argv statments
        size_t outIdx = 1;
        for (size_t i = 1; argv[i] != NULL; i++)
        {
            if (strcmp(argv[i], ">") == 0)
            {
                outIdx = i;
                argv[i] = NULL;
            }
        }
        file_in(argv);
        argv[outIdx] = ">";
        for (size_t i = outIdx; argv[i] != NULL; i++)
        {
            outIdx++;
            argv[i - 1] = argv[i];
        }
        argv[outIdx] == NULL;
        file_out(argv);
    }
    else if (findInput(argv) == 4)
    {
        append(argv);
    }

    // if its not a build in command
    if (!builtin_command(argv))
    {
        if (isFull(jobArray))
        {
            printf("All processes currently busy\n");
            return;
        }

        // Child runs user job
        if ((pid = fork()) == 0)
        {
            setpgid(0, 0);
            // execute job
            if (execvp(argv[0], argv) < 0)
            {
                // if execvp fails, execute execv
                if (execv(argv[0], argv) < 0)
                {
                    printf("%s: Command not found.\n", argv[0]);
                    exit(0);
                }
            }
        }
        // parent waits for fg job
        if (!bg)
        {
            int status;
            // get pid of child
            fg_pid = pid;
            assignJob(jobArray, pid, bg, copy_cmd);
            if (waitpid(pid, &status, (WUNTRACED)) < 0)
            {
                unix_error("waitfg: waitpid error");
            }
            for (size_t i = 0; i < MAXJOB; i++)
            {
                // delete jobs that have finished running
                if (jobArray[i].pid == pid && jobArray[i].state == 1)
                {
                    deleteJob(pid);
                    break;
                }
            }
            // signal is called, reset fg_pid
            fg_pid = 0;
        }
        // otherwise, don't wait for bg job
        else
        {
            assignJob(jobArray, pid, bg, copy_cmd);
        }
    }

    // if there was a file redirect, we would change it back to the original IO
    dup2(file_input, STDIN_FILENO);
    dup2(file_output, STDOUT_FILENO);
}

int main()
{
    char cmdline[MAXLINE];
    // initializing sig handlers
    signal(SIGINT, sigint_handler);
    signal(SIGTSTP, sigstp_handler);
    signal(SIGCHLD, sigchld_handler);
    fillJobArr(jobArray);
    while (1)
    {
        printf("prompt > ");

        fgets(cmdline, MAXLINE, stdin);
        if (feof(stdin))
            exit(0);

        eval(cmdline);
    }

    return 0;
}