#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstdlib>
#include <cerrno>
#include <signal.h>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <fcntl.h>
#include <termios.h>

using namespace std;

volatile sig_atomic_t child_terminated = 0;

void sigchld_handler(int) {
    child_terminated = 1;
}

// trim helpers
static inline void rtrim(string &s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
}
static inline void ltrim(string &s) {
    size_t i = 0;
    while (i < s.size() && isspace((unsigned char)s[i])) ++i;
    if (i) s.erase(0, i);
}
static inline void trim(string &s) { ltrim(s); rtrim(s); }

// Function to split input by spaces
vector<string> parseInput(const string &input) {
    vector<string> tokens;
    stringstream ss(input);
    string word;
    while (ss >> word) tokens.push_back(word);
    return tokens;
}

enum JobStatus { RUNNING, STOPPED, DONE };

struct Job {
    int jid;            // job id (%n)
    pid_t pgid;         // process group id
    string cmd;
    JobStatus status;
};

struct Command {
    vector<string> argv;
    string infile;
    string outfile;
    bool append = false;
};

vector<Job> jobs;
int next_jid = 1;
pid_t shell_pgid;
struct termios shell_tmodes;

// helper: find job by jid or pgid or pid
Job* find_job_by_jid(int jid) {
    for (auto &j : jobs) if (j.jid == jid) return &j;
    return nullptr;
}
Job* find_job_by_pgid(pid_t pgid) {
    for (auto &j : jobs) if (j.pgid == pgid) return &j;
    return nullptr;
}
Job* find_job_by_pid(pid_t pid) {
    pid_t pg = getpgid(pid);
    if (pg < 0) return nullptr;
    return find_job_by_pgid(pg);
}

void remove_job_by_pgid(pid_t pgid) {
    jobs.erase(remove_if(jobs.begin(), jobs.end(),
                         [pgid](const Job &j){ return j.pgid == pgid; }),
               jobs.end());
}

void print_jobs() {
    for (const auto &j : jobs) {
        const char *s = (j.status == RUNNING) ? "Running" : (j.status == STOPPED) ? "Stopped" : "Done";
        cout << "[" << j.jid << "] " << j.pgid << " " << s << "    " << j.cmd << "\n";
    }
}

// reap and update job statuses (called in main loop when signal set)
void update_jobs() {
    int status;
    pid_t pid;
    // loop - handle exited/stopped/continued children
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0) {
        Job *j = find_job_by_pid(pid);
        pid_t pgid = (pid > 0) ? getpgid(pid) : -1;
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (j) {
                // check if any process in group still running - we simplify: remove job and notify
                cout << "\n[" << j->jid << "] " << j->pgid << " Done    " << j->cmd << "\n";
                remove_job_by_pgid(j->pgid);
            } else {
                // orphan child
                // no-op
            }
        } else if (WIFSTOPPED(status)) {
            if (j) {
                j->status = STOPPED;
                cout << "\n[" << j->jid << "] " << j->pgid << " Stopped    " << j->cmd << "\n";
            }
        } else if (WIFCONTINUED(status)) {
            if (j) {
                j->status = RUNNING;
                cout << "\n[" << j->jid << "] " << j->pgid << " Continued    " << j->cmd << "\n";
            }
        }
    }
    child_terminated = 0;
}

vector<Command> buildCommands(const vector<string>& tokens) {
    vector<Command> cmds;
    cmds.emplace_back();
    for (size_t i = 0; i < tokens.size(); ++i) {
        const string &tk = tokens[i];
        if (tk == "|") {
            // start new command
            cmds.emplace_back();
        } else if (tk == "<") {
            if (i + 1 < tokens.size()) {
                cmds.back().infile = tokens[++i];
            }
        } else if (tk == ">") {
            if (i + 1 < tokens.size()) {
                cmds.back().outfile = tokens[++i];
                cmds.back().append = false;
            }
        } else if (tk == ">>") {
            if (i + 1 < tokens.size()) {
                cmds.back().outfile = tokens[++i];
                cmds.back().append = true;
            }
        } else {
            cmds.back().argv.push_back(tk);
        }
    }
    // remove any empty trailing command (e.g., trailing pipe)
    if (!cmds.empty() && cmds.back().argv.empty() && cmds.back().infile.empty() && cmds.back().outfile.empty()) {
        cmds.pop_back();
    }
    return cmds;
}

int runPipeline(vector<Command>& cmds, bool background, const string &raw_cmdline) {
    int n = cmds.size();
    if (n == 0) return -1;

    // Special-case single builtin executed in parent (only when not part of a pipeline)
    if (n == 1 && !background && !cmds[0].argv.empty()) {
        const string &name = cmds[0].argv[0];
        if (name == "cd") {
            const char *path = (cmds[0].argv.size() > 1) ? cmds[0].argv[1].c_str() : getenv("HOME");
            if (!path) path = "/";
            if (chdir(path) != 0) perror("cd");
            return 0;
        } else if (name == "jobs") {
            print_jobs();
            return 0;
        } else if (name == "fg" || name == "bg") {
            // handle in main loop using builtin parsing; here we return to caller to handle
            // but we still support these in runPipeline by doing nothing here - caller handles earlier
        } else if (name == "exit") {
            exit(0);
        }
    }

    // create pipes
    vector<int> pipes;
    if (n > 1) pipes.resize(2 * (n - 1));
    for (int i = 0; i < n - 1; ++i) {
        if (pipe(&pipes[2*i]) < 0) {
            perror("pipe");
            return -1;
        }
    }

    vector<pid_t> pids;
    pid_t pgid = 0;
    for (int i = 0; i < n; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            // cleanup created children
            for (pid_t c : pids) kill(-c, SIGTERM);
            return -1;
        }
        if (pid == 0) {
            // child
            // create/join process group
            if (pgid == 0) setpgid(0, 0);
            else setpgid(0, pgid);

            // set default signal handlers for child
            signal(SIGINT, SIG_DFL);
            signal(SIGTSTP, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            // stdin from previous pipe if not first
            if (i > 0) {
                int in_fd = pipes[2*(i-1)];
                if (dup2(in_fd, STDIN_FILENO) < 0) { perror("dup2"); _exit(1); }
            }
            // stdout to next pipe if not last
            if (i < n-1) {
                int out_fd = pipes[2*i + 1];
                if (dup2(out_fd, STDOUT_FILENO) < 0) { perror("dup2"); _exit(1); }
            }

            // handle input redirection
            if (!cmds[i].infile.empty()) {
                int fd = open(cmds[i].infile.c_str(), O_RDONLY);
                if (fd < 0) { perror(cmds[i].infile.c_str()); _exit(1); }
                if (dup2(fd, STDIN_FILENO) < 0) { perror("dup2"); close(fd); _exit(1); }
                close(fd);
            }

            // handle output redirection
            if (!cmds[i].outfile.empty()) {
                int flags = O_WRONLY | O_CREAT | (cmds[i].append ? O_APPEND : O_TRUNC);
                int fd = open(cmds[i].outfile.c_str(), flags, 0644);
                if (fd < 0) { perror(cmds[i].outfile.c_str()); _exit(1); }
                if (dup2(fd, STDOUT_FILENO) < 0) { perror("dup2"); close(fd); _exit(1); }
                close(fd);
            }

            // close all pipe fds in child
            for (int fd : pipes) close(fd);

            // prepare argv
            if (cmds[i].argv.empty()) _exit(0);
            vector<char*> argv;
            for (auto &s : cmds[i].argv) argv.push_back(const_cast<char*>(s.c_str()));
            argv.push_back(nullptr);

            execvp(argv[0], argv.data());
            perror("exec");
            _exit(EXIT_FAILURE);
        } else {
            // parent
            // establish pgid (set group of child to pgid)
            if (pgid == 0) {
                pgid = pid;
                if (setpgid(pid, pgid) < 0) {
                    // ignore: might already be in pg
                }
            } else {
                setpgid(pid, pgid);
            }
            pids.push_back(pid);
        }
    }

    // parent: close all pipe fds
    for (int fd : pipes) close(fd);

    // foreground handling: give terminal to job's pgid, wait for it to finish/stop
    if (pgid == 0) pgid = pids.empty() ? 0 : pids[0];

    if (background) {
        // add to job list
        Job j;
        j.jid = next_jid++;
        j.pgid = pgid;
        j.cmd = raw_cmdline;
        j.status = RUNNING;
        jobs.push_back(j);
        cout << "[" << j.jid << "] " << j.pgid << " Started\n";
    } else {
        // put job in foreground
        // give terminal control to job
        if (tcsetpgrp(STDIN_FILENO, pgid) < 0) {
            // may fail; continue anyway
        }

        // wait for job: wait on process group
        int status;
        pid_t wpid;
        bool job_stopped = false;
        while (true) {
            wpid = waitpid(-pgid, &status, WUNTRACED);
            if (wpid < 0) {
                if (errno == ECHILD) break;
                if (errno == EINTR) continue;
                break;
            }
            if (WIFSTOPPED(status)) {
                job_stopped = true;
                // add to job list as stopped
                Job j;
                j.jid = next_jid++;
                j.pgid = pgid;
                j.cmd = raw_cmdline;
                j.status = STOPPED;
                jobs.push_back(j);
                cout << "\n[" << j.jid << "] " << j.pgid << " Stopped    " << j.cmd << "\n";
                break;
            }
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                // continue waiting until all in group are reaped
                // continue loop; when no more children, waitpid will return -1/ECHILD and we exit
            }
        }

        // restore terminal to shell
        tcsetpgrp(STDIN_FILENO, shell_pgid);
        // restore shell terminal modes if needed
        tcgetattr(STDIN_FILENO, &shell_tmodes);
    }

    return 0;
}

int parse_job_token(const string &arg) {
    // returns jid if %n form, otherwise 0
    if (arg.empty()) return 0;
    if (arg[0] == '%') {
        return atoi(arg.c_str() + 1);
    }
    return 0;
}

int main() {
    // ensure shell is in its own process group and has control of terminal
    shell_pgid = getpid();
    if (setpgid(shell_pgid, shell_pgid) < 0) {
        // ignore
    }
    // take control of terminal
    tcsetpgrp(STDIN_FILENO, shell_pgid);
    tcgetattr(STDIN_FILENO, &shell_tmodes);

    // ignore interactive job-control signals in shell
    signal(SIGTTOU, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);

    // install SIGCHLD handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    string input;

    while (true) {
        if (child_terminated) update_jobs();

        cout << "myshell> ";
        if (!getline(cin, input)) break;

        trim(input);
        if (input.empty()) continue;

        // detect trailing & for background if present in raw input
        bool background = false;
        if (!input.empty() && input.back() == '&') {
            background = true;
            input.pop_back();
            trim(input);
        }

        vector<string> tokens = parseInput(input);
        if (tokens.empty()) continue;

        // If last token is "&" (user typed separated) handle it
        if (tokens.back() == "&") {
            background = true;
            tokens.pop_back();
            if (tokens.empty()) continue;
        }

        // builtins: jobs, fg, bg, cd, exit handled before pipeline run when appropriate
        if (tokens.size() >= 1) {
            if (tokens[0] == "jobs") {
                print_jobs();
                continue;
            } else if (tokens[0] == "cd") {
                const char *path = (tokens.size() > 1) ? tokens[1].c_str() : getenv("HOME");
                if (!path) path = "/";
                if (chdir(path) != 0) {
                    perror("cd");
                }
                continue;
            } else if (tokens[0] == "exit") {
                exit(0);
            } else if (tokens[0] == "fg" || tokens[0] == "bg") {
                // determine target job
                Job *target = nullptr;
                if (tokens.size() > 1) {
                    // support: %n (job id), n (job id), or pid/pgid
                    if (!tokens[1].empty() && tokens[1][0] == '%') {
                        int jid = atoi(tokens[1].c_str() + 1);
                        if (jid > 0) target = find_job_by_jid(jid);
                    } else {
                        // if token is all digits try as job id first
                        bool all_digits = true;
                        for (char c : tokens[1]) {
                            if (c < '0' || c > '9') { all_digits = false; break; }
                        }
                        if (all_digits) {
                            int jid = atoi(tokens[1].c_str());
                            target = find_job_by_jid(jid);
                            if (!target) {
                                pid_t p = (pid_t)atoi(tokens[1].c_str());
                                if (p != 0) target = find_job_by_pgid(p) ? find_job_by_pgid(p) : find_job_by_pid(p);
                            }
                        } else {
                            pid_t p = (pid_t)atoi(tokens[1].c_str());
                            if (p != 0) target = find_job_by_pgid(p) ? find_job_by_pgid(p) : find_job_by_pid(p);
                        }
                    }
                } else {
                    if (!jobs.empty()) target = &jobs.back();
                }
                
                if (!target) {
                    cerr << tokens[0] << ": no such job\n";
                    continue;
                }
                
                if (tokens[0] == "bg") {
                    // continue in background
                    if (kill(-target->pgid, SIGCONT) < 0) perror("kill(SIGCONT)");
                    target->status = RUNNING;
                    cout << "[" << target->jid << "] " << target->pgid << " Continued in background\n";
                    continue;
                } else {
                    // fg: put job in foreground
                    // give terminal to job pgid
                    if (tcsetpgrp(STDIN_FILENO, target->pgid) < 0) {
                        // ignore
                    }
                    // send SIGCONT to the job's process group
                    if (kill(-target->pgid, SIGCONT) < 0) perror("kill(SIGCONT)");
                    // wait for it
                    int status;
                    pid_t w;
                    while (true) {
                        w = waitpid(-target->pgid, &status, WUNTRACED);
                        if (w < 0) {
                            if (errno == ECHILD) break;
                            if (errno == EINTR) continue;
                            break;
                        }
                        if (WIFSTOPPED(status)) {
                            target->status = STOPPED;
                            cout << "\n[" << target->jid << "] " << target->pgid << " Stopped    " << target->cmd << "\n";
                            break;
                        }
                        if (WIFEXITED(status) || WIFSIGNALED(status)) {
                            // continue until all processes in group handled
                        }
                    }
                    // if job finished, remove it
                    // check if any processes remain in pgid by attempting to send 0 signal
                    if (kill(-target->pgid, 0) < 0) {
                        // likely no such process group -> remove job
                        remove_job_by_pgid(target->pgid);
                    } else {
                        // if stopped we kept it in jobs (status set above)
                    }
                    // restore terminal to shell
                    tcsetpgrp(STDIN_FILENO, shell_pgid);
                    continue;
                }
            }
        }

        // build commands (handles |, <, >, >>)
        vector<Command> cmds = buildCommands(tokens);
        if (cmds.empty()) continue;

        // run pipeline (handles creating job entries for background/stopped)
        runPipeline(cmds, background, input);
    }

    return 0;
}
