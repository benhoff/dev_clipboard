#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Function to print error messages
error() {
    echo "Error: $1"
    exit 1
}

# Check if VERSION file exists
if [ ! -f VERSION ]; then
    error "VERSION file not found."
fi

# Read version from VERSION file
CENTRAL_VERSION=$(cat VERSION)
echo "Central VERSION: $CENTRAL_VERSION"

# Check if dkms.conf exists
if [ ! -f dkms.conf ]; then
    error "dkms.conf file not found."
fi

# Read version from dkms.conf
DKMS_VERSION=$(grep "^PACKAGE_VERSION" dkms.conf | cut -d'=' -f2 | tr -d '"')
echo "dkms.conf PACKAGE_VERSION: $DKMS_VERSION"

# Check if bootstrap.sh exists
if [ ! -f bootstrap.sh ]; then
    error "bootstrap.sh file not found."
fi

# Read version from bootstrap.sh
BOOTSTRAP_VERSION=$(grep "MODULE_VERSION=" bootstrap.sh | cut -d'=' -f2 | tr -d '"')
echo "bootstrap.sh VERSION: $BOOTSTRAP_VERSION"

# Compare versions
if [ "$CENTRAL_VERSION" != "$DKMS_VERSION" ]; then
    error "Version mismatch: VERSION=$CENTRAL_VERSION vs dkms.conf PACKAGE_VERSION=$DKMS_VERSION"
fi

if [ "$CENTRAL_VERSION" != "$BOOTSTRAP_VERSION" ]; then
    error "Version mismatch: VERSION=$CENTRAL_VERSION vs bootstrap.sh VERSION=$BOOTSTRAP_VERSION"
fi

echo "Version consistency check passed."
exit 0

