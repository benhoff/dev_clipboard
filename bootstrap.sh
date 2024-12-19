#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Function to print error messages
error() {
    echo "Error: $1"
    exit 1
}

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Detect the Linux distribution
detect_distro() {
    if [ -f /etc/os-release ]; then
        . /etc/os-release
        DISTRO=$ID
    else
        error "Cannot detect the operating system."
    fi
    echo "Detected Linux distribution: $DISTRO"
}

# Install DKMS and necessary build tools
install_dependencies() {
    case "$DISTRO" in
        ubuntu|debian)
            echo "Updating package lists..."
            sudo apt update
            echo "Installing DKMS and build-essential..."
            sudo apt install -y dkms build-essential git curl
            ;;
        arch)
            echo "Updating package lists..."
            sudo pacman -Syu --noconfirm
            echo "Installing DKMS and base-devel..."
            sudo pacman -S --noconfirm dkms base-devel git curl
            ;;
        fedora)
            echo "Updating package lists..."
            sudo dnf check-update || true
            echo "Installing DKMS and necessary tools..."
            sudo dnf install -y dkms @development-tools git curl
            ;;
        *)
            error "Unsupported Linux distribution: $DISTRO"
            ;;
    esac
}

# Download the GitHub project
download_project() {
    GITHUB_REPO="https://github.com/benhoff/dev_clipboard.git" # Replace with the actual GitHub repository URL
    MODULE_NAME="clipboard"       # Replace with the actual module name
    MODULE_VERSION="3.4"

    TEMP_DIR=$(mktemp -d)
    echo "Downloading the project from GitHub..."

    if command_exists git; then
        git clone "$GITHUB_REPO" "$TEMP_DIR/$MODULE_NAME-$MODULE_VERSION"
    else
        echo "git not found. Attempting to download via curl..."
        # Assuming the repo provides a tarball; adjust the URL as needed
        curl -L "$GITHUB_REPO/archive/refs/tags/v$MODULE_VERSION.tar.gz" | tar -xz -C "$TEMP_DIR" --strip-components=1
    fi

    echo "Project downloaded to $TEMP_DIR/$MODULE_NAME-$MODULE_VERSION"
}

# Compare two version strings
version_greater_equal() {
    # Returns 0 if $1 >= $2, 1 otherwise
    # Uses sort -V for version comparison
    [ "$(printf '%s\n%s' "$1" "$2" | sort -V | head -n1)" = "$2" ]
}

# Check if the module is installed and determine if an update is needed
check_and_update_module() {
    EXISTING_VERSION=$(dkms status | grep "^clipboard" | cut -d'/' -f2 | cut -d',' -f1)
    
    if [ -z "$EXISTING_VERSION" ]; then
        echo "Module '${MODULE_NAME}' is not installed. Proceeding with installation."
        return 0
    else
        echo "Module '${MODULE_NAME}' is already installed with version ${EXISTING_VERSION}."
        if version_greater_equal "$EXISTING_VERSION" "$MODULE_VERSION"; then
            echo "A newer or equal version is already installed. No update needed."
            exit 0
        else
            echo "Installed version is older than desired version. Proceeding with update."
            # Remove the existing module version
            sudo dkms remove -m "$MODULE_NAME" -v "$EXISTING_VERSION" --all
        fi
    fi
}

# Add, build, and install the module using DKMS
setup_dkms() {
    DEST_DIR="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"

    echo "Copying project to $DEST_DIR..."
    sudo cp -r "$TEMP_DIR/$MODULE_NAME-$MODULE_VERSION" "$DEST_DIR"

    echo "Adding the module to DKMS..."
    sudo dkms add -m "$MODULE_NAME" -v "$MODULE_VERSION"

    echo "Building the module..."
    sudo dkms build -m "$MODULE_NAME" -v "$MODULE_VERSION"

    echo "Installing the module..."
    sudo dkms install -m "$MODULE_NAME" -v "$MODULE_VERSION"
}

# Configure the module to load on startup
configure_startup() {
    MODULE_CONF="/etc/modules-load.d/${MODULE_NAME}.conf"
    echo "Configuring the module to load on startup..."

    # Add the module name to the modules-load.d configuration
    echo "$MODULE_NAME" | sudo tee "$MODULE_CONF" >/dev/null

    # Enable the module immediately if it's not already loaded
    if ! lsmod | grep -q "^${MODULE_NAME}"; then
        echo "Loading the module..."
        sudo modprobe "$MODULE_NAME"
    fi

    echo "Module configuration complete."
}

# Cleanup temporary files
cleanup() {
    echo "Cleaning up temporary files..."
    rm -rf "$TEMP_DIR"
}

# Main execution flow
main() {
    detect_distro
    install_dependencies
    download_project
    check_and_update_module
    setup_dkms
    configure_startup
    cleanup
    echo "DKMS setup and module installation complete."
}

# Execute the main function
main

