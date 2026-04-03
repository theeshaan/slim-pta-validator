# A utility that instruments IR with points-to information
# to validate the pts information at runtime
# First cut: only support flow-insensitive pts info

# command-line args: <SLIM IR file> <Pts file>

# assumption about the format of the pts file:
# each line has the format:
# pointervarName_functionName var1 var2 var3 ... (where vari = pointeei_functionName)

import argparse
import os
from pathlib import Path

##### Design the data structure #####
# design : map indexed by pointervarName_functionName, returns a
# set of addresses that the pointer variable can point to
pts_info = {}

###### Parse command-line arguments ######
parser = argparse.ArgumentParser()
parser.add_argument("ir_file") # File containing SLIM IR of the program
parser.add_argument("pts_file") # File containing pts info for the program
args = parser.parse_args()
ir_file = Path(args.ir_file)
pts_file = Path(args.pts_file)
if not ir_file.exists():
    print(f"Error: IR file {ir_file} does not exist.")
    exit(1)
if not pts_file.exists():
    print(f"Error: PTS file {pts_file} does not exist.")
    exit(1)
if not ir_file.is_file():
    print(f"Error: IR file {ir_file} is not a file.")
    exit(1)
if not pts_file.is_file():
    print(f"Error: PTS file {pts_file} is not a file.")
    exit(1)

##### open the points to file and load the info into data structures #####
with open(pts_file, 'r') as f:
    for line in f:
        # split the line into parts based on spaces
        parts = line.split()
        # the first part is pointervarName_functionName
        ptr_var_func = parts[0]
        # the remaining parts are pointees, split by spaces
        pointees = parts[1:]
        # add the info to the data structure
        pts_info[ptr_var_func] = set(pointees)

##### open the IR file and instrument it with the pts info #####
# for each instruction in the IR, if it contains a pointer dereference,
# then insert an assertion that checks if the address being dereferenced
# is in the pts set for that pointer variable
# create a new file to write the instrumented IR to
instrumented_ir_file = ir_file.parent / (ir_file.stem + "_instrumented" + ir_file.suffix)
with open(ir_file, 'r') as f_in, open(instrumented_ir_file, 'w') as f_out:
    for line in f_in:
        # write the original line to the output file
        f_out.write(line)
        # check if the line contains a pointer dereference
        