#!/bin/sh
# Pulls the official Khronos Vulkan-Headers (needed for vulkan_core.h and the
# vk_video sub-headers it includes). Only needs to run once.
set -e
cd "$(dirname "$0")/.."
if [ -d vulkan_inc/vulkan ]; then
    echo "Headers already present at vulkan_inc/. Delete that dir to re-fetch."
    exit 0
fi
mkdir -p vulkan_inc
curl -sL -o /tmp/vk-headers.tar.gz \
    https://codeload.github.com/KhronosGroup/Vulkan-Headers/tar.gz/refs/heads/main
tar -xzf /tmp/vk-headers.tar.gz -C /tmp
cp -r /tmp/Vulkan-Headers-main/include/vulkan vulkan_inc/vulkan
cp -r /tmp/Vulkan-Headers-main/include/vk_video vulkan_inc/vk_video
rm -rf /tmp/Vulkan-Headers-main /tmp/vk-headers.tar.gz
echo "Headers installed at vulkan_inc/"
