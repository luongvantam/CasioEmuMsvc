#!/usr/bin/env bash
set -e

# Curated HSL colors for nice output
INFO='\033[1;34m'
SUCCESS='\033[1;32m'
WARNING='\033[1;33m'
ERROR='\033[1;31m'
NC='\033[0m'

echo -e "${INFO}---> Cleaning build-ios folder...${NC}"
rm -rf build-ios

echo -e "${INFO}---> Configuring CMake for iOS Xcode Project...${NC}"
# Use STATIC_LIBRARY for try compile type to avoid code signing issues during CMake checks
# Target arm64 architecture on iphoneos SDK
cmake -S . -B build-ios -G Ninja \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

echo ""
echo -e "${SUCCESS}====================================================${NC}"
echo -e "${SUCCESS}  Xcode Project Generated Successfully in 'build-ios/' ✔${NC}"
echo -e "${SUCCESS}====================================================${NC}"
echo ""
echo -e "You can open the project in Xcode using this command:"
echo -e "  ${INFO}open build-ios/GAME.xcodeproj${NC}"
echo ""
echo -e "Or double-click the file in Finder."
