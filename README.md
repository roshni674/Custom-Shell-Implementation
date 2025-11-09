🐚 Custom Unix Shell Implementation

A simple Unix-like shell built in C++ as part of an Operating Systems (Linux) assignment.
This shell can execute user commands, manage processes, and support features like redirection, piping, and job control — providing hands-on experience with core system programming concepts.

🚀 Features

Command Execution: Run basic Linux commands (like ls, pwd, cat, etc.)

Process Management: Execute programs in foreground or background.

Input/Output Redirection: Use >, <, and >> to redirect output or input.

Piping: Chain multiple commands using | (e.g., ls | grep cpp).

Job Control: View background jobs, bring them to foreground, or resume them.

⚙️ Technologies Used

Language: C++

OS: Linux (via WSL / Ubuntu)

Concepts: Forking, execvp, waitpid, pipes, signal handling

🧩 How to Run

Clone this repository:

git clone https://github.com/roshni674/Custom-Shell-Implementation.git
cd Custom-Shell-Implementation


Compile the shell:

g++ main.cpp -o myshell


Run it:

./myshell

🧠 Example Commands
myshell> ls
myshell> echo "Hello World"
myshell> cat file.txt > output.txt
myshell> ps | grep bash
myshell> sleep 10 &
myshell> jobs
myshell> fg 1
myshell> exit

📅 Project Structure
File	Description
main.cpp	Core shell source code
files.txt	Sample file for testing redirection
result.txt	Example output file
myshell	Compiled executable
✨ Learning Outcomes

Deep understanding of process creation (fork/exec)

Handling I/O redirection and pipes

Basics of job control and signals

Improved familiarity with Linux system calls and terminal-based applications

👩‍💻 Author

Roshni
4th Year Computer Science Engineering Student
