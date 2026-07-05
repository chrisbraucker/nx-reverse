#!/bin/bash
# -+- coding: UTF-8 -+-

docker volume create codex-data || true
docker volume create nx-reverse-history || true
