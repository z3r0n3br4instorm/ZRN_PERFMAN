#!/usr/bin/env python3
"""
train_model.py  -  Train the Next Window Prediction Neural Network on ~/.zrn_switch_data.csv
and export binary weights to ~/.zrn_model_weights.bin
"""

import os
import sys
import csv
import math
import random
import struct

DATA_PATH = os.path.expanduser("~/.zrn_switch_data.csv")
WEIGHTS_PATH = os.path.expanduser("~/.zrn_model_weights.bin")

NN_INPUT_DIM = 16
NN_HIDDEN1 = 32
NN_HIDDEN2 = 16
NN_OUTPUT_DIM = 64

def hash_comm(comm):
    h = 5381
    for c in comm:
        h = (((h << 5) + h) + ord(c)) & 0xFFFFFFFFFFFFFFFF
    return (h % 10000) / 10000.0

def train():
    if not os.path.exists(DATA_PATH):
        print(f"[!] Data file not found: {DATA_PATH}")
        sys.exit(1)

    records = []
    app_counts = {}
    with open(DATA_PATH, "r", encoding="utf-8", errors="ignore") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                records.append({
                    "ts": int(row["timestamp"]),
                    "from_pid": int(row["from_pid"]),
                    "from_comm": row["from_comm"],
                    "to_pid": int(row["to_pid"]),
                    "to_comm": row["to_comm"],
                    "duration": int(row["duration_ms"])
                })
                to_comm = row["to_comm"]
                app_counts[to_comm] = app_counts.get(to_comm, 0) + 1
            except (ValueError, KeyError):
                continue

    if len(records) < 10:
        print("[!] Not enough switch data to train (minimum 10 records required).")
        sys.exit(1)

    sorted_apps = sorted(app_counts.items(), key=lambda x: x[1], reverse=True)
    app_names = [a[0] for a in sorted_apps[:NN_OUTPUT_DIM]]
    app_to_idx = {name: i for i, name in enumerate(app_names)}

    print(f"[*] Loaded {len(records)} switch samples across {len(app_names)} unique target apps.")

    X = []
    Y = []
    for i in range(len(records)):
        r = records[i]
        target = r["to_comm"]
        if target not in app_to_idx:
            continue
        y = app_to_idx[target]

        feat = [0.0] * NN_INPUT_DIM
        feat[0] = hash_comm(r["from_comm"])
        time_min = (r["ts"] // 60) % 1440
        feat[1] = time_min / 1440.0

        rate = 0
        for j in range(max(0, i - 30), i):
            if records[j]["ts"] >= r["ts"] - 60:
                rate += 1
        feat[2] = min(1.0, rate / 60.0)

        for k in range(13):
            hist_idx = i - 1 - k
            if hist_idx >= 0:
                feat[3 + k] = hash_comm(records[hist_idx]["to_comm"])

        X.append(feat)
        Y.append(y)

    random.seed(42)
    def init_matrix(rows, cols):
        scale = math.sqrt(2.0 / cols)
        return [[random.gauss(0, scale) for _ in range(cols)] for _ in range(rows)]

    W1 = init_matrix(NN_HIDDEN1, NN_INPUT_DIM)
    b1 = [0.0] * NN_HIDDEN1
    W2 = init_matrix(NN_HIDDEN2, NN_HIDDEN1)
    b2 = [0.0] * NN_HIDDEN2
    W3 = init_matrix(NN_OUTPUT_DIM, NN_HIDDEN2)
    b3 = [0.0] * NN_OUTPUT_DIM

    lr = 0.05
    epochs = 60
    batch_size = 32

    print(f"[*] Training 3-layer neural network ({NN_INPUT_DIM}->{NN_HIDDEN1}->{NN_HIDDEN2}->{NN_OUTPUT_DIM})...")
    for epoch in range(epochs):
        indices = list(range(len(X)))
        random.shuffle(indices)
        total_loss = 0.0
        correct = 0

        for start in range(0, len(X), batch_size):
            batch_idx = indices[start:start + batch_size]
            dW1 = [[0.0]*NN_INPUT_DIM for _ in range(NN_HIDDEN1)]
            db1 = [0.0]*NN_HIDDEN1
            dW2 = [[0.0]*NN_HIDDEN1 for _ in range(NN_HIDDEN2)]
            db2 = [0.0]*NN_HIDDEN2
            dW3 = [[0.0]*NN_HIDDEN2 for _ in range(NN_OUTPUT_DIM)]
            db3 = [0.0]*NN_OUTPUT_DIM

            for idx in batch_idx:
                x = X[idx]
                y = Y[idx]

                z1 = [b1[i] + sum(W1[i][j]*x[j] for j in range(NN_INPUT_DIM)) for i in range(NN_HIDDEN1)]
                h1 = [max(0.0, v) for v in z1]

                z2 = [b2[i] + sum(W2[i][j]*h1[j] for j in range(NN_HIDDEN1)) for i in range(NN_HIDDEN2)]
                h2 = [max(0.0, v) for v in z2]

                z3 = [b3[i] + sum(W3[i][j]*h2[j] for j in range(NN_HIDDEN2)) for i in range(NN_OUTPUT_DIM)]

                max_z = max(z3)
                exp_z = [math.exp(v - max_z) for v in z3]
                sum_exp = sum(exp_z)
                probs = [v / sum_exp for v in exp_z]

                loss = -math.log(max(1e-12, probs[y]))
                total_loss += loss
                if probs.index(max(probs)) == y:
                    correct += 1

                dz3 = [probs[i] - (1.0 if i == y else 0.0) for i in range(NN_OUTPUT_DIM)]
                for i in range(NN_OUTPUT_DIM):
                    db3[i] += dz3[i]
                    for j in range(NN_HIDDEN2):
                        dW3[i][j] += dz3[i] * h2[j]

                dh2 = [sum(dz3[i] * W3[i][j] for i in range(NN_OUTPUT_DIM)) for j in range(NN_HIDDEN2)]
                dz2 = [dh2[j] if z2[j] > 0 else 0.0 for j in range(NN_HIDDEN2)]
                for i in range(NN_HIDDEN2):
                    db2[i] += dz2[i]
                    for j in range(NN_HIDDEN1):
                        dW2[i][j] += dz2[i] * h1[j]

                dh1 = [sum(dz2[i] * W2[i][j] for i in range(NN_HIDDEN2)) for j in range(NN_HIDDEN1)]
                dz1 = [dh1[j] if z1[j] > 0 else 0.0 for j in range(NN_HIDDEN1)]
                for i in range(NN_HIDDEN1):
                    db1[i] += dz1[i]
                    for j in range(NN_INPUT_DIM):
                        dW1[i][j] += dz1[i] * x[j]

            N = len(batch_idx)
            for i in range(NN_OUTPUT_DIM):
                b3[i] -= lr * (db3[i] / N)
                for j in range(NN_HIDDEN2):
                    W3[i][j] -= lr * (dW3[i][j] / N)
            for i in range(NN_HIDDEN2):
                b2[i] -= lr * (db2[i] / N)
                for j in range(NN_HIDDEN1):
                    W2[i][j] -= lr * (dW2[i][j] / N)
            for i in range(NN_HIDDEN1):
                b1[i] -= lr * (db1[i] / N)
                for j in range(NN_INPUT_DIM):
                    W1[i][j] -= lr * (dW1[i][j] / N)

        if (epoch + 1) % 20 == 0 or epoch == epochs - 1:
            acc = (correct / len(X)) * 100.0
            print(f"  -> Epoch {epoch+1:2d}/{epochs}: Loss = {total_loss/len(X):.4f}, Accuracy = {acc:.1f}%")

    with open(WEIGHTS_PATH, "wb") as f:
        w1_flat = [val for row in W1 for val in row]
        f.write(struct.pack(f"{len(w1_flat)}f", *w1_flat))
        f.write(struct.pack(f"{len(b1)}f", *b1))

        w2_flat = [val for row in W2 for val in row]
        f.write(struct.pack(f"{len(w2_flat)}f", *w2_flat))
        f.write(struct.pack(f"{len(b2)}f", *b2))

        w3_flat = [val for row in W3 for val in row]
        f.write(struct.pack(f"{len(w3_flat)}f", *w3_flat))
        f.write(struct.pack(f"{len(b3)}f", *b3))

        f.write(struct.pack("i", len(app_names)))
        for name in app_names:
            encoded = name.encode("utf-8")[:63]
            padded = encoded.ljust(64, b'\x00')
            f.write(padded)

    print(f"[+] Exported model weights to {WEIGHTS_PATH}")

if __name__ == "__main__":
    train()
