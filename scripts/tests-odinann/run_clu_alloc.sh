#!/bin/bash
# ============================================================
# Clu-Alloc Experiment Script (Section 3.2.2)
# ============================================================
# This script runs the Clu-Alloc experiment comparing:
# - Append-Only (baseline)
# - Random-Alloc
# - Clu-Alloc (proposed method)
#
# ========== DATA PATH CONFIGURATION ==========
# MODIFY THESE PATHS TO MATCH YOUR ENVIRONMENT:

# Data files
DATA_BIN="/mnt/xiaoxuanx/dataset/sift/sift_base.fbin"    # Binary data file for insertion
QUERY_FILE="/mnt/xiaoxuanx/dataset/sift/sift_query.fbin" # Query file

# Index files (pre-built with 50% data)
INDEX_PREFIX="/mnt/xiaoxuanx/dataset/sift/sift_index"     # Index prefix path
# Output directory for results
OUTPUT_DIR="/mnt/xiaoxuanx/dataset/sift/draw"

# ========== END DATA PATH CONFIGURATION ==========

# Experiment parameters
TYPE="float"           # Data type: int8, uint8, or float
L_DISK=128             # L parameter for disk index
VECS_PER_STEP=1000000  # 1M vectors per step
NUM_STEPS=10           # 10 steps = 10M total insertions
INSERT_THREADS=10      # Number of insertion threads
SEARCH_THREADS=32      # Number of search threads
SEARCH_MODE=0          # 0=BEAM_SEARCH, 1=PAGE_SEARCH, 2=PIPE_SEARCH
RECALL_AT=10           # recall@10
BEAM_WIDTH=4           # Beam width for building
SEARCH_BEAM_WIDTH=4    # Beam width for search
MEM_L=0                # L for in-memory index (0 = not used)
L_SEARCH=20            # L parameter for search

CWD=$(pwd)
mkdir -p $OUTPUT_DIR

echo "============================================================"
echo "Clu-Alloc Experiment (Section 3.2.2)"
echo "============================================================"
echo "Data file: $DATA_BIN"
echo "Query file: $QUERY_FILE"
echo "Index prefix: $INDEX_PREFIX"
echo "Output directory: $OUTPUT_DIR"
echo "Vecs per step: $VECS_PER_STEP"
echo "Num steps: $NUM_STEPS"
echo "============================================================"

# Function to run experiment with specific strategy
run_strategy() {
    STRATEGY=$1
    STRATEGY_NAME=$2
    
    echo ""
    echo "=========================================="
    echo "Running $STRATEGY_NAME strategy..."
    echo "=========================================="
    
    # Clean up any previous shadow files
    rm -f ${INDEX_PREFIX}_shadow* 2>/dev/null
    rm -f ${INDEX_PREFIX}_merge* 2>/dev/null
    
    # Run the experiment
    build/tests/test_clu_alloc \
        $TYPE \
        $DATA_BIN \
        $L_DISK \
        $VECS_PER_STEP \
        $NUM_STEPS \
        $INSERT_THREADS \
        $SEARCH_THREADS \
        $SEARCH_MODE \
        $INDEX_PREFIX \
        $QUERY_FILE \
        $RECALL_AT \
        $BEAM_WIDTH \
        $SEARCH_BEAM_WIDTH \
        $MEM_L \
        $L_SEARCH \
        $OUTPUT_DIR \
        $STRATEGY \
        |& tee $OUTPUT_DIR/log_${STRATEGY_NAME}.txt
    
    # Clean up shadow files
    rm -f ${INDEX_PREFIX}_shadow* 2>/dev/null
    rm -f ${INDEX_PREFIX}_merge* 2>/dev/null
}

# Run all three strategies
run_strategy 0 "append"
run_strategy 1 "random"
run_strategy 2 "cluster"

echo ""
echo "============================================================"
echo "Experiment Complete!"
echo "Results saved to: $OUTPUT_DIR"
echo "  - clu_alloc_append.csv"
echo "  - clu_alloc_random.csv"
echo "  - clu_alloc_cluster.csv"
echo ""
echo "Run the plotting script to generate figures:"
echo "  python3 $OUTPUT_DIR/plot_clu_alloc.py"
echo "============================================================"
