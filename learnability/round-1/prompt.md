You are taking part in a test of how well a new programming language can be learned from its reference alone. The language is Mo. Nothing about it is on the web, and you have not seen it before.

1. Download Mo's reference with this command, then read all of it:
   amp files get REFERENCE_URL -o mo-reference.md
2. Using only that reference, write the twelve programs described below in Mo. Each is one complete Mo module in its own file in a folder named answers, named as in the list: answers/greet.mo, answers/wc.mo, and so on.
3. Don't search the web, don't read any other files, and don't run any programs other than the commands needed to download the reference and save your files. There is no Mo compiler, so don't try to run or test the programs. Don't ask questions: where a task leaves something open, choose sensibly and keep going.
4. When all twelve files are written, call the thread_file_url tool for them, with threadID set to your own thread's ID (the environment variable AMP_THREAD_ID holds it). Then send the twelve URLs it returns to thread T-01a12ad3-7d47-70cc-af83-c91a5ba423bd with send_thread_message, with one line per file: the file name, a space, and its URL. Then stop. Don't create any other threads.

The programs:
1. greet: Greets the name given on the command line, or the world.
2. wc: Counts the lines, words and bytes in each file named on the command line, like Unix wc, with a total when there is more than one file. A file that can't be read is reported, and the exit status is then 1.
3. find: Prints the lines of files that contain a phrase, with each file's name and the line's number. Usage: find [--ignore-case] [--count] PHRASE FILE... --ignore-case (-i) ignores ASCII case; --count (-c) prints only how many lines matched in each file. The exit status is 0 when a line matched, 1 when none did, and 2 on a usage error or a file that can't be read.
4. expenses: Totals a CSV file of expenses by category, largest first. The first line is a header; every other line is date,category,amount, with amounts such as 12.50. A malformed line is reported with its line number and skipped, and the exit status is then 1.
5. json: Reads JSON from standard input and prints it laid out with two-space indentation; or, given a path such as items.0.name, prints only the value at that path. Exponents and \u escapes need not be supported.
6. todo: A to-do list kept in the file todo.txt: "todo add TEXT", "todo list" (numbered from 1), "todo done N" and "todo remove N".
7. account: The core of a bank account: open, deposit, withdraw and transfer, where a withdrawal can't overdraw. Its type's insides are hidden, and it has promises and examples, including that a transfer neither makes nor loses money.
8. rate-limit: A token-bucket rate limiter with one bucket per client: a bucket holds up to a capacity of tokens and refills at a rate per second; a request takes a token or is refused with how long to wait. Time is passed in. Promises and tests included.
9. job-queue: The core of a job queue: submit, lease (the oldest ready job, for a given time), acknowledge, fail, and expire leases; a job is tried at most three times, then dead. The queue is a value, and promises say no operation loses a job.
10. kv: The core of a Redis-like store: parses SET, GET, DEL, INCR and EXPIRE command lines and applies them to a store at a given time, giving a reply and the new store; INCR reports overflow. A main serves commands from standard input.
11. notes-tool: The handler of an MCP tool that searches the Markdown notes in one read-only folder: it takes a typed input (a query and a limit up to 100) and returns typed hits (file, line number, text) and whether they were cut at the limit, or a typed error.
12. inventory: Generic helpers (the top N by a key, grouping by a key, removing duplicates) and a text table with aligned columns, used on a list of products.
