import os
import sys
import numpy as np

def process_trace_folder(folder):
    for fname in sorted(os.listdir(folder)):
        if fname.endswith(".pitree-trace"):
            fpath = os.path.join(folder, fname)
            try:
                bw_values = []
                with open(fpath, "r") as f:
                    for line in f:
                        parts = line.strip().split()
                        if len(parts) == 2:
                            try:
                                bw = float(parts[1])
                                bw_values.append(bw)
                            except ValueError:
                                continue
                if bw_values:
                    mean_bw = np.mean(bw_values)
                    std_bw = np.std(bw_values)
                    print(f"{fname}: mean={mean_bw:.3f} Mbps, std={std_bw:.3f} Mbps")
            except Exception as e:
                print(f"Error reading {fname}: {e}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: python {sys.argv[0]} <trace_folder>")
        sys.exit(1)

    folder = sys.argv[1]
    process_trace_folder(folder)
