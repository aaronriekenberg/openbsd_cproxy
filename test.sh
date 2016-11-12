#!/bin/sh

BASE_LOCAL_PORT=5000
NUM_LOCAL_PORTS=10
BASE_REMOTE_PORT=10000
NUM_REMOTE_PORTS=1
CMD="./cproxy"
i=0
while [ ${i} -lt ${NUM_LOCAL_PORTS} ]; do
  CMD="${CMD} -l 0.0.0.0:$((${BASE_LOCAL_PORT} + ${i}))"
  i=$((${i} + 1))
done
i=0
while [ ${i} -lt ${NUM_REMOTE_PORTS} ]; do
  CMD="${CMD} -r 127.0.0.1:$((${BASE_REMOTE_PORT} + ${i}))"
  i=$((${i} + 1))
done
CMD="${CMD} -c 2000"
echo "${CMD}"
$CMD
