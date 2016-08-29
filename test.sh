#!/bin/sh

BASE_PORT=5000
NUM_PORTS=10
CMD="./cproxy"
i=0
while [ ${i} -lt ${NUM_PORTS} ]; do
  CMD="${CMD} -l 0.0.0.0:$((${BASE_PORT} + ${i}))"
  i=$((${i} + 1))
done
CMD="${CMD} -r 127.0.0.1:10000 -c 2000"
echo "${CMD}"
$CMD
