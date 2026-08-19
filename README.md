## How To Build

Run .\build.ps1 in the root dir, you can find the exe after building at build\bin\

## Shipping

Engine projects cook to a bin folder with app.exe and other required files

## Layout

    src/          editor, compiler, scene and asset systems

    data/         node, type, component and asset descriptors

    tests/        doctest suite

    third_party/  imgui, imgui-node-editor, toml++, doctest
