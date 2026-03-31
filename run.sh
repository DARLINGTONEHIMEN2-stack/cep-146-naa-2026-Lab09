#!/usr/bin/env bash
# run.sh - Build and launch the TTC Transit Departure Board

set -e

echo "Building TTC Departure Board..."
make all

echo "Launching TTC Departure Board..."
echo "(Press Q to quit, D to toggle direction filter)"
sleep 1
./departures
