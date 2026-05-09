#!/usr/bin/env bash
set -euo pipefail

port="${1:-/dev/ttyACM0}"
names=(alice bob charlie dan eve frank grace heidi)
counter=0

while true; do
  users=$(((counter % ${#names[@]}) + 1))
  ip="192.168.1.$((42 + (counter % 40)))"
  mode="client"

  if (( counter % 20 >= 15 )); then
    mode="ap"
    ip=""
  fi

  user_names=""
  for ((i = 0; i < users && i < 5; i++)); do
    if [[ -n "$user_names" ]]; then
      user_names+=","
    fi
    user_names+="\"${names[$i]}\""
  done

  printf '{"service":"MURMUR","mode":"%s","wifi":"StatusNet Demo","ip":"%s","users":%d,"user_names":[%s]}\n' \
    "$mode" \
    "$ip" \
    "$users" \
    "$user_names" > "$port"

  counter=$((counter + 1))
  sleep 0.25
done
