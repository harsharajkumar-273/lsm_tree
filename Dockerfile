# Use Ubuntu 22.04 as the base image for a modern Linux kernel environment
#
# ---------------------------------------------------------------------------
# RUNTIME REQUIREMENT: this image needs elevated privileges.
#
#   docker run --rm --privileged lsm-engine
#
# The engine issues io_uring submissions and opens its write-ahead log with
# O_DIRECT. Docker's default seccomp profile blocks io_uring_setup(2), so
# without --privileged (or --cap-add=SYS_ADMIN together with a seccomp profile
# that permits io_uring) the WAL fails at construction and every test errors.
#
# Nothing in this Dockerfile can grant those privileges — they are supplied at
# `docker run`, not at build time. This note exists because the failure is
# otherwise opaque: the image builds cleanly and the container starts, then
# dies on the first write with an error that does not mention capabilities.
# ---------------------------------------------------------------------------
FROM ubuntu:22.04

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install build essentials, CMake, liburing development headers, and pkg-config
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    liburing-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /usr/src/lsm_tree

# Copy the project files into the container
COPY . .

# Create a build directory, run CMake, and compile the project
RUN mkdir -p build && \
    cd build && \
    cmake .. && \
    make

# Set the entry point to run the tests and benchmarks
#
# Invoked through `bash` rather than relying on the executable bit. The bit is
# tracked in git as mode 100755 and COPY preserves it, so `./run_all.sh` works
# today — but a checkout through an archive export, a Windows filesystem, or a
# restrictive umask can drop it, and the resulting "permission denied" gives no
# hint about why. Calling the interpreter directly removes that dependency.
CMD ["bash", "run_all.sh"]
