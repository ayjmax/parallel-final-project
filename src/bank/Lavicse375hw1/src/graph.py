from typing import List
import matplotlib.pyplot as plt

class Trial:
    threads: int
    key_max: int
    iters: int
    time: int
    time_serial: int
    def __init__(self, line: str):
        tokens = line.split(",")
        self.threads = int(tokens[0])
        self.key_max = int(tokens[1])
        self.iters = int(tokens[2])
        self.time = int(tokens[3])
        self.time_serial = int(tokens[4])

add = "With vector parallelism"
# filename = [("balpercent5_improvedsharedlock.txt", "After shared lock"), ("fivepercentbalavx.txt", "Before shared lock"), ("serial_baseline.txt", "Serial (no vector instructions)")]
filename = [()]
trials: List[List[Trial]] = []
x_axis = "threads"
y_axis = "time"
consistent_values = [["key_max", None], ["iters", None]]
for file in filename:    
    f = open(file[0], "r")
    header = f.readline()
    data = f.read().strip()
    f.close()

    times = []
    lines = data.split("\n")
    for line in lines:
        if line == "" or line[0] == "#":
            continue
        trial = Trial(line)
        times.append(trial)
        for i, c in enumerate(consistent_values):
            if c[1] == None:
                c[1] = eval(f"trial.{c[0]}")
            elif c[1] != eval(f"trial.{c[0]}"):
                print("Error, inconsistent trial comparison")
                exit(0)
    trials.append(times)

title = f"{x_axis} vs {y_axis} ({add})"
title += "\n"
title += f"(Starting at iters={trials[0][0].iters} key_max={trials[0][0].key_max} threads={trials[0][0].threads})"
plt.title(title)
plt.xlabel(x_axis)
plt.ylabel(y_axis)
for i, trial in enumerate(trials):
    x = []
    y = []
    for times in trial:
        x.append(eval(f"times.{x_axis}"))
        y.append(eval(f"times.{y_axis}"))
    plt.plot(x, y, label=filename[i][1])
plt.legend()
plt.savefig("shared_lock_comparison_no_baseline.png")
