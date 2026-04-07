# ChampSim

![GitHub](https://img.shields.io/github/license/ChampSim/ChampSim)
![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/ChampSim/ChampSim/test.yml)
![GitHub forks](https://img.shields.io/github/forks/ChampSim/ChampSim)
[![Coverage Status](https://coveralls.io/repos/github/ChampSim/ChampSim/badge.svg?branch=develop)](https://coveralls.io/github/ChampSim/ChampSim?branch=develop)

ChampSim is a trace-based microarchitecture simulator developed for academic research and simulation competitions. This fork is used as part of the ECE382N Final Project (Spring 2026) and is configured to run on **TACC Lonestar6** via [AutoChamp](https://github.com/Capheuss/autochamp) for automated multi-binary, multi-workload simulation.

For questions about ChampSim's general usage, refer to the [upstream Discussions tab](https://github.com/ChampSim/ChampSim/discussions). For issues specific to this fork, open an issue here.

---

## Citation

If you use ChampSim in your work, please cite:

> Gober, N., Chacon, G., Wang, L., Gratz, P. V., Jimenez, D. A., Teran, E., Pugsley, S., & Kim, J. (2022). *The Championship Simulator: Architectural Simulation for Education and Competition.* https://doi.org/10.48550/arXiv.2210.14324

---

## Repository Structure

```
ChampSim/
├── src/                    # ChampSim core source
├── inc/                    # Header files
├── prefetcher/             # Prefetcher implementations
├── branch/                 # Branch predictor implementations
├── replacement/            # Cache replacement policy implementations
├── btb/                    # Branch target buffer implementations
├── tracer/                 # Trace generation utilities
├── config/                 # Configuration scripts
├── docs/                   # Documentation
├── autochamp/              # AutoChamp submodule for automated launching
├── autochamp_config/       # AutoChamp configuration files and lists
│   ├── autochamp-config.cfg
│   ├── binary_list.txt
│   ├── build_list.txt
│   ├── cvp_subset.txt
│   └── configurations/     # Per-binary JSON config files
├── champsim_config.json    # Example fully-specified configuration
├── config.sh               # Configuration script
└── Makefile
```

---

## Setup

### 1. Download Dependencies

ChampSim uses [vcpkg](https://vcpkg.io) to manage dependencies, included as a submodule:

```bash
git submodule update --init
vcpkg/bootstrap-vcpkg.sh
vcpkg/vcpkg install
```

### 2. Configure and Build

ChampSim is configured via a JSON file. See `champsim_config.json` for a fully-specified example — all fields are optional and fall back to defaults if omitted.

```bash
./config.sh <configuration_file>
make
```

---

## Downloading Traces

SPEC CPU traces for the 3rd Data Prefetching Championship (DPC-3) are available at:
- **DPC-3 traces:** https://dpc3.compas.cs.stonybrook.edu/champsim-traces/speccpu/
- **CRC-2 traces:** http://bit.ly/2t2nkUj

For this project, traces are downloaded to the final project repository's `traces/` directory via a SLURM batch job. See the [Final Project README](https://github.com/Capheuss/ECE382N-FINAL-PROJECT-SP26-cmr4983-gwl459) for details.

> Storage is kindly provided by Daniel Jimenez (Texas A&M University) and Mike Ferdman (Stony Brook University). It is recommended to maintain your own local copy of traces in case these links change.

---

## Running Simulations

### Manual

```bash
bin/champsim --warmup-instructions 200000000 --simulation-instructions 500000000 ~/path/to/traces/600.perlbench_s-210B.champsimtrace.xz
```

Note: instruction counts reflect the number of *retired* instructions. Statistics printed at the end cover the simulation phase only.

### Automated (Recommended)

For launching multiple binaries across multiple workloads on Lonestar6, use AutoChamp:

```bash
python3 autochamp/auto-champ.py -f autochamp_config/autochamp-config.cfg -l
```

See the [AutoChamp README](https://github.com/Capheuss/autochamp/blob/main/README.md) for full setup and usage instructions.

---

## Adding Custom Components

### Prefetcher

```bash
mkdir prefetcher/mypref
cp prefetcher/no_l2c/no.cc prefetcher/mypref/mypref.cc
# Edit mypref.cc, then add to your config JSON:
# { "L2C": { "prefetcher": "mypref" } }
./config.sh <configuration_file>
make
```

### Branch Predictor / Replacement Policy / BTB

Follow the same pattern — copy an existing implementation from the relevant directory (`branch/`, `replacement/`, `btb/`), modify it, reference it in your JSON config, and rebuild.

---

## Creating Traces

Tracing utilities are provided in the `tracer/` directory for users who want to generate traces from their own programs.

---

## Evaluation

ChampSim reports **IPC (Instructions Per Cycle)** as its primary performance metric, along with cache, branch, and memory statistics at the end of each simulation run. With `--json` enabled, these are also written as structured JSON for use with AutoChamp's collect feature.