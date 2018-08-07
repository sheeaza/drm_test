# drm_test

## build
1. If you are using cross compile toolchain first souce enviroment, like:
```
. ~/project/sdk_imx8qm/environment-setup-aarch64-poky-linux
```

2. under drm_test directory run:
```
mkdir build && cd build
```

3. build
```
cmake ../ && make
```

4. the binary is under ```build/src/drm_test```, copy it to your target and run, it should show some color block. If your target is running some display servers, first stop them, like: ```systemctl stop weston```.
