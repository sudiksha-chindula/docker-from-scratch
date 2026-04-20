# Docksmith

Docksmith is a simplified Docker-like image builder and runtime for Linux. It is a single CLI binary (`docksmith`) that stores all state under `~/.docksmith/`.

## Prerequisites

1. Linux (kernel 4.0+ recommended)
2. Root privileges (or equivalent capabilities such as `CAP_SYS_ADMIN`)
3. Toolchain and libs:
   - `gcc`
   - `make`
   - `libarchive-dev`
   - `libssl-dev`
   - `libcjson-dev`

Install on Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y gcc make libarchive-dev libssl-dev libcjson-dev
```

## Build

```bash
make
```

This produces `./docksmith`.

## First-time setup: import a base image

Docksmith is offline after import. You must import a base rootfs tar first.

### Option A (recommended): import an `alpine-python:3.18` rootfs

Create a tarball on a workstation (example using Docker only to prepare the tar):

```bash
docker run --name ds-base --rm -d alpine:3.18 sh -c 'apk add --no-cache python3 && sleep 3600'
docker export ds-base > alpine-python-3.18.tar
docker stop ds-base
```

Import into Docksmith:

```bash
sudo ./docksmith import -t alpine-python:3.18 ./alpine-python-3.18.tar
```

## Sample app build and run

`sample-app/Docksmithfile`:

```dockerfile
FROM alpine-python:3.18
ENV GREETING=Hello
WORKDIR /app
COPY main.py /app/main.py
RUN chmod +x /app/main.py
CMD ["python3", "main.py"]
```

Build:

```bash
sudo ./docksmith build -t sample:latest sample-app
```

List images:

```bash
./docksmith images
```

Run with image CMD:

```bash
sudo ./docksmith run sample:latest
```

Run with ENV override:

```bash
sudo ./docksmith run -e GREETING=Hi sample:latest
```

Run with explicit command override:

```bash
sudo ./docksmith run sample:latest python3 /app/main.py
```

Remove image:

```bash
./docksmith rmi sample:latest
```

## Cache demo

Run the same build twice:

```bash
sudo ./docksmith build -t sample:latest sample-app
sudo ./docksmith build -t sample:latest sample-app
```

The second build should show cache hits for layer-producing steps.

Disable cache reads/writes for one build:

```bash
sudo ./docksmith build --no-cache -t sample:latest sample-app
```

## Troubleshooting

- `Error: docksmith must be run as root...`  
  Run build/run/import with `sudo`.

- `unshare (needs root or user namespaces)`  
  Run on Linux with required namespace support and privileges (a Linux VM is recommended).

- `base image '<name>:<tag>' not found in local store`  
  Import it first with `docksmith import -t <name:tag> <rootfs.tar>`.

- `CMD must use JSON array form`  
  Use `CMD ["prog", "arg1"]`, not shell form.

- `COPY source '<src>' escapes build context`  
  Keep COPY sources inside the build context directory.
