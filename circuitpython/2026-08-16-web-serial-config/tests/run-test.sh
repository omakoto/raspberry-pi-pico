#!/bin/bash

set -e

cd "$(dirname "$0")"/.. || exit 1

npm test
