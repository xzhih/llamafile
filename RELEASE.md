# Making a Llamafile Release

There are a few steps in making a Llamafile release which will be detailed in this document.

The two primary artifacts of the release are the `llamafile-<version>.zip` and the binaries for the GitHub release.

## Release Process

Note: Steps 2 and 3 are only needed if you are making a new release of the ggml-cuda.so and ggml-rocm.so shared libraries. You only need to do this when you are making changes to the CUDA code or the API's surrounding it. Otherwise you can use the previous release of the shared libraries.

### Preparing the Build Environment

Before building, ensure all dependencies are initialized and configured:

```sh
make setup
```

This initializes git submodules (e.g., whisper.cpp) and applies llamafile patches. The patches integrate dependencies with llamafile's build system and add llamafile-specific functionality.

### Release Steps

1. Update the version number in `version.h`
2. Build the ggml-cuda.so and ggml-rocm.so shared libraries on Linux. You need to do this for Llamafile and LocalScore. Llamafile uses TINYBLAS as a default and LocalScore uses CUBLAS as a default for CUDA.
    - For Llamafile you can do this by running the script `./llamafile/cuda.sh` and `./llamafile/rocm.sh` respectively.
    - For LocalScore you can do this by running the script `./localscore/cuda.sh`.
    - The files will be built and placed your home directory.
3. Build the ggml-cuda.dll and ggml-rocm.dll shared libraries on Windows. You need to do this for Llamafile and LocalScore.
    - You can do this by running the script `./llamafile/cuda.bat` and `./llamafile/rocm.bat` respectively.
    - For LocalScore you can do this by running the script `./localscore/cuda.bat`.
    - The files will be built and placed in the `build/release` directory.
4. Build the project with `make -j8`
5. Install the built project to your /usr/local/bin directory with `sudo make install PREFIX=/usr/local`

### Llamafile Release Zip

The easiest way to create the release zip is to:

`make install PREFIX=<preferred_dir>/llamafile-<version>`

This now stages the core release binaries and helper scripts into `<preferred_dir>/llamafile-<version>/bin`,
including `llamafile`, `llama-server`, `whisperfile`, `whisper-server`, `stream`, `mic2txt`, `mic2raw`,
`zipalign`, `llamafile-convert`, and `llamafile-upgrade-engine`.

After the directory is created, you will want to bundle the built shared libraries into the release binaries that need
runtime GPU backends, such as:

- `llamafile`
- `whisperfile`

You can do this for each binary with a command like the following:

Note: You MUST put the shared libraries in the same directory as the binary you are creating.

For llamafile and whisperfile you can do the following:

`zipalign -j0 llamafile ggml-cuda.so ggml-rocm.so ggml-cuda.dll ggml-rocm.dll`
`zipalign -j0 whisperfile ggml-cuda.so ggml-rocm.so ggml-cuda.dll ggml-rocm.dll`

If you build additional binaries outside the default install set, package them the same way after copying them into
`<path_to>/llamafile-<version>/bin`.

The zip is structured as follows.

```
llamafile-<version>
|-- README.md
|-- bin
|   |-- llama-server
|   |-- llamafile
|   |-- llamafile-convert
|   |-- llamafile-upgrade-engine
|   |-- mic2raw
|   |-- mic2txt
|   |-- stream
|   |-- whisper-server
|   |-- whisperfile
|   `-- zipalign
```

Before you zip the directory, you will want to remove the shared libraries from the directory.

`rm *.so *.dll`

You can zip the directory with the following command:

`zip -r llamafile-<version>.zip llamafile-<version>`

### Llamafile Release Binaries

The release binaries are the files staged into `<preferred_dir>/llamafile-<version>/bin` by `make install`.

At minimum, upload the binaries you want to support from that directory together with `llamafile-<version>.zip`.
If you also built platform-specific GPU shared libraries, bundle them into the corresponding binaries with `zipalign`
before uploading the release artifacts.
