FROM apache/beam_python3.12_sdk:latest

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    python3-dev \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/bloomfilter
COPY . /opt/bloomfilter

RUN python -m pip install -U pip
RUN python -m pip install -v ./python

RUN python - <<'PY'
import url_bloom_native

print(url_bloom_native.__doc__)
print(url_bloom_native.__file__)
PY
