#!/bin/bash

# Script to create a portable macOS directory bundle for HD Mapping
# This bundles all executables and dependencies into a simple directory structure
# that can be copied to /Applications or any location

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to find a dylib in the build directory
find_dylib_in_build() {
    local dylib_name="$1"
    
    # Search in build/lib first
    if [ -e "build/lib/$dylib_name" ]; then
        echo "build/lib/$dylib_name"
        return
    fi

    # Dependency fixups revisit bundled libraries. Avoid traversing the whole
    # build tree (including Go caches) again for each already copied library.
    if [ -e "build/HDMapping/lib/$dylib_name" ]; then
        echo "build/HDMapping/lib/$dylib_name"
        return
    fi
    
    # Search recursively in build directory (including symlinks)
    local found=$(find build -name "$dylib_name" \( -type f -o -type l \) 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        echo "$found"
        return
    fi
    
    echo ""
}

# Resolve a dylib's runpath before flattening the dependency tree into lib/.
resolve_dylib_runpath() {
    local dylib_path="$1"
    local relative_dep="$2"
    local loader_dir=$(dirname "$dylib_path")
    local runpath
    while IFS= read -r runpath; do
        case "$runpath" in
            @loader_path) runpath="$loader_dir" ;;
            @loader_path/*) runpath="$loader_dir/${runpath#@loader_path/}" ;;
            /*) ;;
            *) continue ;;
        esac
        if [ -f "$runpath/$relative_dep" ]; then
            printf '%s\n' "$runpath/$relative_dep"
            return 0
        fi
    done < <(otool -l "$dylib_path" | awk '
        $1 == "cmd" { is_rpath = ($2 == "LC_RPATH") }
        is_rpath && $1 == "path" {
            sub(/^[[:space:]]*path /, "")
            sub(/ \(offset [0-9]+\)$/, "")
            print
        }')
    return 1
}

# Function to copy a dylib and fix its install name
bundle_dylib() {
    local dylib_path="$1"
    local dylib_name=$(basename "$dylib_path")
    local dest="build/HDMapping/lib/$dylib_name"
    
    # Skip if already copied
    if [ -e "$dest" ]; then
        return
    fi
    
    # If path doesn't exist, try to find it in build directory
    if [ ! -e "$dylib_path" ]; then
        dylib_path=$(find_dylib_in_build "$dylib_name")
        if [ -z "$dylib_path" ]; then
            return
        fi
    fi
    
    # If it's a symlink, copy the target and create the symlink
    if [ -L "$dylib_path" ]; then
        local target=$(readlink "$dylib_path")
        local target_name=$(basename "$target")
        local dir=$(dirname "$dylib_path")
        
        # If target is relative, resolve it
        if [[ "$target" != /* ]]; then
            target="$dir/$target"
        fi
        
        # Bundle the target first
        if [ -e "$target" ]; then
            bundle_dylib "$target"
        fi
        
        # Create symlink in destination
        if [ ! -e "$dest" ] && [ -e "build/HDMapping/lib/$target_name" ]; then
            cd "build/HDMapping/lib"
            ln -sf "$target_name" "$dylib_name"
            cd - > /dev/null
            echo "  ✓ Linked: $dylib_name -> $target_name"
        fi
        return
    fi
    
    # Copy the dylib
    cp "$dylib_path" "$dest"
    chmod +w "$dest"
    
    # Change its install name to use @loader_path (for dylibs referencing other dylibs)
    install_name_tool -id "@loader_path/$dylib_name" "$dest" 2>/dev/null || true
    
    echo "  ✓ Bundled: $dylib_name"
    
    # Recursively bundle dependencies of this dylib
    local deps=$(otool -L "$dylib_path" | grep -v ":" | awk '{print $1}' | grep -v "^/usr/lib" | grep -v "^/System")
    local loader_dir=$(dirname "$dylib_path")
    for dep in $deps; do
        # Resolve against the original dylib while its source path is known.
        if [[ "$dep" == "@loader_path/"* ]]; then
            dep="$loader_dir/${dep#@loader_path/}"
        elif [[ "$dep" == "@rpath/"* ]]; then
            local resolved
            if resolved=$(resolve_dylib_runpath "$dylib_path" "${dep#@rpath/}"); then
                dep="$resolved"
            fi
        fi
        local dep_name=$(basename "$dep")
        if [[ "$dep" == *".dylib"* ]]; then
            # Try the original path first, then search
            if [ -f "$dep" ]; then
                bundle_dylib "$dep"
            else
                local found_path=$(find_dylib_in_build "$dep_name")
                if [ -n "$found_path" ]; then
                    bundle_dylib "$found_path"
                fi
            fi
        fi
    done
}

# Function to fix dylib references in a binary
fix_binary_dylibs() {
    local binary="$1"
    local binary_name=$(basename "$binary")
    local is_dylib=false
    
    # Check if this is a dylib (in lib/) or executable (in bin/)
    if [[ "$binary" == *"/lib/"* ]]; then
        is_dylib=true
    fi
    
    # Get all non-system dylib dependencies (including @rpath and @loader_path references)
    local deps=$(otool -L "$binary" | grep -v ":" | awk '{print $1}' | grep -v "^/usr/lib/" | grep -v "^/System/")
    
    for dep in $deps; do
        local dep_name=""
        
        # Handle @rpath references
        if [[ "$dep" == "@rpath/"* ]]; then
            dep_name=$(basename "$dep")
            
            # Try to find this dylib in build directory
            local found_path=$(find_dylib_in_build "$dep_name")
            if [ -n "$found_path" ]; then
                # Bundle the dylib
                bundle_dylib "$found_path"
            fi
            
            # Always change reference to use appropriate path
            if [ "$is_dylib" = true ]; then
                # For dylibs in lib/, use @loader_path
                install_name_tool -change "$dep" "@loader_path/$dep_name" "$binary" 2>/dev/null || true
            else
                # For executables in bin/, use @executable_path/../lib
                install_name_tool -change "$dep" "@executable_path/../lib/$dep_name" "$binary" 2>/dev/null || true
            fi
        elif [[ "$dep" == "@loader_path/"* ]]; then
            # Already uses @loader_path - ensure the dylib is bundled and update if needed
            dep_name=$(basename "$dep")
            local found_path=$(find_dylib_in_build "$dep_name")
            if [ -n "$found_path" ]; then
                bundle_dylib "$found_path"
            fi
            
            # Ensure correct path for context (executable vs dylib)
            if [ "$is_dylib" = false ]; then
                # For executables, change to @executable_path/../lib
                install_name_tool -change "$dep" "@executable_path/../lib/$dep_name" "$binary" 2>/dev/null || true
            elif [[ "$dep" != "@loader_path/$dep_name" ]]; then
                install_name_tool -change "$dep" "@loader_path/$dep_name" "$binary" 2>/dev/null || true
            fi
        elif [[ "$dep" == "@executable_path/"* ]]; then
            # Already uses @executable_path
            dep_name=$(basename "$dep")
            local found_path=$(find_dylib_in_build "$dep_name")
            if [ -n "$found_path" ]; then
                bundle_dylib "$found_path"
            fi
        elif [[ "$dep" == *".dylib"* ]] && [ -f "$dep" ]; then
            dep_name=$(basename "$dep")
            
            # Bundle the dylib if not already done
            bundle_dylib "$dep"
            
            # Change reference in binary to use appropriate path
            if [ "$is_dylib" = true ]; then
                # For dylibs in lib/, use @loader_path
                install_name_tool -change "$dep" "@loader_path/$dep_name" "$binary" 2>/dev/null || true
            else
                # For executables in bin/, use @executable_path/../lib
                install_name_tool -change "$dep" "@executable_path/../lib/$dep_name" "$binary" 2>/dev/null || true
            fi
        fi
    done
}

echo -e "${GREEN}=====================================${NC}"
echo -e "${GREEN}HD Mapping - Creating Directory Bundle${NC}"
echo -e "${GREEN}=====================================${NC}"

# Configuration
BUNDLE_NAME="HDMapping"
BUNDLE_DIR="build/${BUNDLE_NAME}"
BUILD_BIN_DIR="build/bin"
BUILD_LIB_DIR="build/lib"

# Check if build directory exists
if [ ! -d "$BUILD_BIN_DIR" ]; then
    echo -e "${RED}Error: $BUILD_BIN_DIR not found. Please build the project first.${NC}"
    exit 1
fi

echo -e "${YELLOW}Step 1: Creating bundle directory structure...${NC}"
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/bin"
mkdir -p "$BUNDLE_DIR/lib"
echo "  ✓ Created $BUNDLE_DIR/bin/"
echo "  ✓ Created $BUNDLE_DIR/lib/"

echo -e "${YELLOW}Step 2: Copying all executables to bin/...${NC}"
BINARY_COUNT=0
for binary in "$BUILD_BIN_DIR"/*; do
    if [ -f "$binary" ] && [ -x "$binary" ]; then
        binary_name=$(basename "$binary")
        case "$binary_name" in
            hdmapping_mcp|hdmapping_coordinator|hdmapping_worker)
                grep -q '^1$' build/hdmapping-mcp-enabled.txt || continue
                ;;
        esac
        cp "$binary" "$BUNDLE_DIR/bin/"
        chmod +x "$BUNDLE_DIR/bin/$binary_name"
        echo "  ✓ $binary_name"
        BINARY_COUNT=$((BINARY_COUNT + 1))
    fi
done
if grep -q '^1$' build/hdmapping-mcp-enabled.txt; then
    for binary_name in hdmapping_mcp hdmapping_coordinator hdmapping_worker; do
        if [ ! -x "$BUNDLE_DIR/bin/$binary_name" ]; then
            echo "Error: MCP enabled but $binary_name was not built"
            exit 1
        fi
    done
fi
cp "build/bin/hdmapping-orb-mur.fbow" "$BUNDLE_DIR/bin/" || exit 1
if ! IFS= read -r PROJ_DATABASE < build/hdmapping-proj-database.txt || [ ! -f "$PROJ_DATABASE" ]; then
    echo "Error: the PROJ database selected by CMake is missing. Configure with the complete PROJ package."
    exit 1
fi
# The bundled dylib resolves its database relative to lib/.
mkdir -p "$BUNDLE_DIR/share/proj"
cp "$PROJ_DATABASE" "$BUNDLE_DIR/share/proj/proj.db"
echo "  Total: $BINARY_COUNT executables copied"

echo -e "${YELLOW}Step 3: Detecting and bundling dynamic libraries...${NC}"

# Special case: Bundle FreeGLUT (built by the project but not auto-detected)
FREEGLUT_DYLIB=$(find build/3rdparty/freeglut -name "libglut.*.*.*.dylib" -type f 2>/dev/null | head -1)
if [ -f "$FREEGLUT_DYLIB" ]; then
    bundle_dylib "$FREEGLUT_DYLIB"
fi

# Fix dylibs for all binaries
for binary in "$BUNDLE_DIR/bin"/*; do
    if [ -f "$binary" ] && [ -x "$binary" ]; then
        fix_binary_dylibs "$binary"
    fi
done

# Count bundled dylibs
DYLIB_COUNT=$(ls -1 "$BUNDLE_DIR/lib" 2>/dev/null | wc -l)
echo "  Total: $DYLIB_COUNT dynamic libraries bundled"

echo -e "${YELLOW}Step 4: Fixing inter-dylib dependencies...${NC}"
# Fix references between dylibs in lib/
for dylib in "$BUNDLE_DIR/lib"/*.dylib; do
    if [ -f "$dylib" ]; then
        fix_binary_dylibs "$dylib"
    fi
done

# Create symlinks for versioned libraries (e.g., libglut.3.13.0.dylib -> libglut.3.dylib)
for dylib in "$BUNDLE_DIR/lib"/*.dylib; do
    if [ -f "$dylib" ]; then
        dylib_name=$(basename "$dylib")
        # Match pattern like libXXX.#.#.#.dylib -> create libXXX.#.dylib symlink
        if [[ $dylib_name =~ ^(.+)\.([0-9]+)\.([0-9]+)\.([0-9]+)\.dylib$ ]]; then
            base="${BASH_REMATCH[1]}"
            major="${BASH_REMATCH[2]}"
            symlink_name="${base}.${major}.dylib"
            if [ ! -e "$BUNDLE_DIR/lib/$symlink_name" ]; then
                cd "$BUNDLE_DIR/lib"
                ln -sf "$dylib_name" "$symlink_name"
                cd - > /dev/null
                echo "  ✓ Created symlink: $symlink_name -> $dylib_name"
            fi
        fi
    fi
done

echo "  ✓ Fixed inter-dylib dependencies"

echo -e "${YELLOW}Step 5: Copying README.txt...${NC}"
# Get the directory where the script is located
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
README_SOURCE="${SCRIPT_DIR}/bundle_README.txt"

if [ -f "$README_SOURCE" ]; then
    cp "$README_SOURCE" "$BUNDLE_DIR/README.txt"
    echo "  ✓ README.txt copied"
else
    echo -e "${RED}Warning: bundle_README.txt not found in $SCRIPT_DIR${NC}"
    echo "  ⚠ README.txt not copied"
fi

echo -e "${YELLOW}Step 6: Verifying and signing bundled Mach-O files...${NC}"
# install_name_tool can invalidate a Mach-O signature. Repair it after all fixups.
for binary in "$BUNDLE_DIR/bin/"* "$BUNDLE_DIR/lib/"*.dylib; do
    if [ -f "$binary" ] && [ ! -L "$binary" ] && file -b "$binary" | grep -q '^Mach-O'; then
        if ! codesign --verify --strict "$binary" 2>/dev/null; then
            codesign --force --sign - "$binary"
            codesign --verify --strict "$binary"
        fi
    fi
done
echo "  ✓ Bundled Mach-O signatures verified"

echo ""
echo -e "${GREEN}=====================================${NC}"
echo -e "${GREEN}Bundle created successfully!${NC}"
echo -e "${GREEN}=====================================${NC}"
echo ""
echo "Location: $BUNDLE_DIR"
echo "Executables: $BINARY_COUNT"
echo "Bundled libraries: $DYLIB_COUNT"
echo ""
echo "Structure:"
echo "  $BUNDLE_DIR/"
echo "    ├── bin/       ($BINARY_COUNT executables)"
echo "    ├── lib/       ($DYLIB_COUNT libraries)"
echo "    ├── share/proj/proj.db"
echo "    └── README.txt"
echo ""
echo "To test the bundle:"
echo "  $BUNDLE_DIR/bin/hd_mapping_launcher_gui"
echo ""
echo "To install to Applications:"
echo "  cp -r $BUNDLE_DIR /Applications/"
echo ""
echo "To add to PATH:"
echo "  echo 'export PATH=\"/Applications/$BUNDLE_NAME/bin:\$PATH\"' >> ~/.zshrc"
echo ""
echo "To create a DMG for distribution:"
echo "  ./macos_bundle_toolkit/create_dir_bundle_dmg.sh"
echo ""
