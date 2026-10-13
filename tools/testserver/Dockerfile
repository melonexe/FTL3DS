# FTL3DS test server: builds the headless game (test/headless) from each new commit on GitHub, runs every
# scenario test plus seed sweeps, and shows the results in a web page. See tools/testserver/README.md.
FROM python:3.12-slim

RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential libpng-dev zlib1g-dev git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY server.py index.html ./

# /data holds ftl.dat (yours: mount it, it's never in the image), the repository cache and results.
VOLUME /data
EXPOSE 8080
CMD ["python", "-u", "server.py"]
