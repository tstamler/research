import os, time, re, string
from time import gmtime, strftime

input_trace = [ "ll_0p_key", "ll_10p_key", "ll_20p_key", "ll_30p_key","ll_40p_key","ll_50p_key",
                "ll_60p_key", "ll_70p_key", "ll_80p_key", "ll_90p_key","ll_100p_key" ]

#input_trace = [ "ll_0p_key" ]

num_cpu = ["1", "10", "20", "30", "40"]

outputfile = "log_"+ strftime("%Y-%m-%d_%H_%M_%S", gmtime())

for trace in input_trace:
    for ncpu in num_cpu:
        os.system("echo \"#define NUM_CPU ("+ncpu+")\" > config.h")
        os.system("echo \"#define TRACE_FILE \\\"../ll_trace/"+trace+"\\\"\" >> config.h")
        if trace == "":
            os.system("echo \"#define READ_ONLY\" >> config.h")

        os.system("rm ./bench; make; ./bench >> "+outputfile)
        print trace+" w/ "+ ncpu + "cpus done"
        time.sleep(2)
