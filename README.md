# Keygen - X11 Key Generator

A simple graphical key generator application using the X11 library like the good
old keygen from the 1990's era but for UNIX/Linux :)

## Requirements

- GCC compiler
- X11 development libraries (`libx11-dev` on Debian/Ubuntu, `libX11-devel` on Fedora/Red Hat)

## Building

```bash
make
```

## Usage

```bash
./keygen
```

## Features

- Generates random 16-character alphanumeric keys
- Keys are formatted with dashes every 4 characters (e.g., `ABCD-1234-EFGH-5678`)
- Click the "Generate" button to create a new key
- Press `Q` or `Escape` to quit

## How it works

The application creates an X11 window with:
- A title at the top
- The generated key displayed in the center
- A "Generate" button at the bottom

Each time you click the button, a new random key is generated using `rand()` seeded with the current time.
