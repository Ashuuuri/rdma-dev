#!/usr/bin/env python3
import sys
import csv
import argparse
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

def load_csv(filename):
    with open(filename) as f:
        return list(csv.DictReader(f))

def fmt_size(n):
    n = int(n)
    if n >= 1 << 20: return f"{n >> 20}M"
    if n >= 1 << 10: return f"{n >> 10}K"
    return str(n)

def plot_lat(data_by_label, output_prefix):
    fig, ax = plt.subplots(figsize=(10, 6))

    for label, rows in data_by_label.items():
        rows = [r for r in rows if r['mode'] == 'lat' and r['avg_us']]
        if not rows:
            continue
        by_op = {}
        for r in rows:
            by_op.setdefault(r['op'], []).append(r)

        for op, op_rows in by_op.items():
            op_rows.sort(key=lambda r: int(r['size']))
            sizes = [int(r['size'])   for r in op_rows]
            avg   = [float(r['avg_us']) for r in op_rows]
            p99   = [float(r['p99_us']) for r in op_rows]
            tag   = f"{op} ({label})" if len(data_by_label) > 1 else op
            ax.plot(sizes, avg, 'o-',  label=f"{tag} avg")
            ax.plot(sizes, p99, 's--', label=f"{tag} p99", alpha=0.7)

    ax.set_xscale('log', base=2)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: fmt_size(x)))
    ax.set_xlabel('Message Size')
    ax.set_ylabel('Latency (µs)')
    ax.set_title('RDMA Latency')
    ax.legend()
    ax.grid(True, alpha=0.3)

    out = f"{output_prefix}_lat.png"
    fig.savefig(out, dpi=150, bbox_inches='tight')
    print(f"saved: {out}")
    plt.close(fig)

def plot_bw(data_by_label, output_prefix):
    fig, ax = plt.subplots(figsize=(10, 6))

    for label, rows in data_by_label.items():
        rows = [r for r in rows if r['mode'] == 'bw' and r['gbps']]
        if not rows:
            continue
        by_op = {}
        for r in rows:
            by_op.setdefault(r['op'], []).append(r)

        for op, op_rows in by_op.items():
            op_rows.sort(key=lambda r: int(r['size']))
            sizes = [int(r['size'])   for r in op_rows]
            gbps  = [float(r['gbps']) for r in op_rows]
            tag   = f"{op} ({label})" if len(data_by_label) > 1 else op
            ax.plot(sizes, gbps, 'o-', label=tag)

    ax.set_xscale('log', base=2)
    ax.xaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: fmt_size(x)))
    ax.set_xlabel('Message Size')
    ax.set_ylabel('Bandwidth (GB/s)')
    ax.set_title('RDMA Bandwidth')
    ax.legend()
    ax.grid(True, alpha=0.3)

    out = f"{output_prefix}_bw.png"
    fig.savefig(out, dpi=150, bbox_inches='tight')
    print(f"saved: {out}")
    plt.close(fig)

def main():
    parser = argparse.ArgumentParser(description='Plot RDMA benchmark results')
    parser.add_argument('files', nargs='+', help='CSV result files')
    parser.add_argument('--output', default='bench',
                        help='output filename prefix (default: bench)')
    args = parser.parse_args()

    data_by_label = {f: load_csv(f) for f in args.files}
    all_rows = [r for rows in data_by_label.values() for r in rows]

    if any(r['mode'] == 'lat' for r in all_rows):
        plot_lat(data_by_label, args.output)
    if any(r['mode'] == 'bw'  for r in all_rows):
        plot_bw(data_by_label, args.output)

if __name__ == '__main__':
    main()
