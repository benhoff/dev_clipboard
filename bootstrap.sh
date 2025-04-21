#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

# Function to print error messages
error() {
    echo "Error: $1" >&2
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

install_dependencies() {
    local missing=0

    case "$DISTRO" in
        ubuntu|debian)
            # List of required packages
            local pkgs=(dkms build-essential git curl)
            for pkg in "${pkgs[@]}"; do
                if ! dpkg -s "$pkg" &>/dev/null; then
                    echo "$pkg is not installed."
                    missing=1
                fi
            done
            if [ $missing -eq 0 ]; then
                echo "All dependencies are already installed."
                return 0
            fi
            echo "Installing missing dependencies..."
            sudo apt install -y "${pkgs[@]}"
            ;;
        arch)
            local pkgs=(dkms base-devel git curl)
            for pkg in "${pkgs[@]}"; do
                if ! pacman -Qi "$pkg" &>/dev/null; then
                    echo "$pkg is not installed."
                    missing=1
                fi
            done
            if [ $missing -eq 0 ]; then
                echo "All dependencies are already installed."
                return 0
            fi
            echo "Installing missing dependencies..."
            sudo pacman -S --noconfirm "${pkgs[@]}"
            ;;
        fedora)
            # For Fedora, check individual packages
            local pkgs=(dkms git curl)
            for pkg in "${pkgs[@]}"; do
                if ! rpm -q "$pkg" &>/dev/null; then
                    echo "$pkg is not installed."
                    missing=1
                fi
            done
            # Check for gcc as a proxy for development tools
            if ! rpm -q gcc &>/dev/null; then
                echo "gcc (development tools) is not installed."
                missing=1
            fi
            if [ $missing -eq 0 ]; then
                echo "All dependencies are already installed."
                return 0
            fi
            echo "Installing missing dependencies..."
            sudo dnf install -y dkms @development-tools git curl
            ;;
        *)
            echo "Unsupported Linux distribution: $DISTRO" >&2
            return 1
            ;;
    esac
}

# Download the GitHub project
download_project() {
    GITHUB_REPO="https://github.com/benhoff/dev_clipboard.git" # Replace with the actual GitHub repository URL
    MODULE_NAME="clipboard"       # Replace with the actual module name
    MODULE_VERSION="3.10"

    TEMP_DIR=$(mktemp -d)
    echo "Downloading the project from GitHub..."

    if command_exists git; then
        git clone --branch "v${MODULE_VERSION}" "$GITHUB_REPO" "$TEMP_DIR/${MODULE_NAME}-${MODULE_VERSION}" || {
            echo "Tag v${MODULE_VERSION} not found. Cloning default branch."
            git clone "$GITHUB_REPO" "$TEMP_DIR/${MODULE_NAME}-${MODULE_VERSION}"
        }
    else
        echo "git not found. Attempting to download via curl..."
        # Assuming the repo provides a tarball; adjust the URL as needed
        curl -L "$GITHUB_REPO/archive/refs/tags/v${MODULE_VERSION}.tar.gz" | tar -xz -C "$TEMP_DIR" --strip-components=1
    fi

    echo "Project downloaded to $TEMP_DIR/${MODULE_NAME}-${MODULE_VERSION}"
}

# Compare two version strings
version_greater_equal() {
    # Returns 0 if $1 >= $2, 1 otherwise
    # Uses sort -V for version comparison
    [ "$(printf '%s\n%s' "$1" "$2" | sort -V | head -n1)" = "$2" ]
}

# Check if the module is installed and determine if an update is needed
check_and_update_module() {
    # Get all installed versions of the module
    INSTALLED_VERSIONS=$(dkms status | grep "^${MODULE_NAME}/" | awk -F'/' '{print $2}' | awk -F',' '{print $1}' | tr '\n' ' ')

    if [ -z "$INSTALLED_VERSIONS" ]; then
        echo "Module '${MODULE_NAME}' is not installed. Proceeding with installation."
        return 0
    else
        echo "Module '${MODULE_NAME}' is already installed with version(s): ${INSTALLED_VERSIONS}."
        # Check if any installed version is greater than or equal to desired
        for ver in $INSTALLED_VERSIONS; do
            if version_greater_equal "$ver" "$MODULE_VERSION"; then
                echo "A newer or equal version (${ver}) is already installed. No update needed."
                exit 0
            fi
        done
        echo "Installed version(s) are older than desired version. Proceeding with update."
        # Remove all existing versions
        for ver in $INSTALLED_VERSIONS; do
            echo "Removing existing module version: ${MODULE_NAME}/${ver}"
            sudo dkms remove -m "$MODULE_NAME" -v "$ver" --all || {
                error "Failed to remove existing module version: ${MODULE_NAME}/${ver}"
            }
        done
    fi
}

# Add, build, and install the module using DKMS
setup_dkms() {
    DEST_DIR="/usr/src/${MODULE_NAME}-${MODULE_VERSION}"

    echo "Copying project to $DEST_DIR..."
    sudo cp -r "$TEMP_DIR/${MODULE_NAME}-${MODULE_VERSION}" "$DEST_DIR" || {
        error "Failed to copy project to $DEST_DIR"
    }

    # Verify dkms.conf exists
    if [ ! -f "$DEST_DIR/dkms.conf" ]; then
        error "dkms.conf not found in $DEST_DIR. Ensure the project is DKMS-compatible."
    fi

    echo "Adding the module to DKMS..."
    sudo dkms add -m "$MODULE_NAME" -v "$MODULE_VERSION" || {
        error "Failed to add module to DKMS."
    }

    echo "Building the module..."
    sudo dkms build -m "$MODULE_NAME" -v "$MODULE_VERSION" || {
        error "Failed to build the DKMS module."
    }

    echo "Installing the module..."
    sudo dkms install -m "$MODULE_NAME" -v "$MODULE_VERSION" || {
        error "Failed to install the DKMS module."
    }
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
        sudo modprobe "$MODULE_NAME" || {
            error "Failed to load the module ${MODULE_NAME}."
        }
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

