# scpm-repo

The official package repository for Linux Nexus, an hardware-agnostic, source-based distribution which promotes the "Build It Yourself" (BIY) philosophy.

Instead of dealing with huge binaryblob downloads or rebuilding your entire system from scratch, with `scpm`, you can fetch versioned source tars over `libcurl`, natively track your dependencies tree and compile your favorite software straight on your target machine.

Channels

You can either use the preffered stable stream (recommended for most users) or the bleeding-edge one (for those who want to track `main/master` commits):

Stable Stream: versioned tagged commits, for stable releases. Bleeding-Edge Stream: `main/master` branch, for latest developments.

```

# /etc/scpm/scpm.conf

source https://aurifeen.github.io/scpm-repo/packages-stable.json

# source https://aurifeen.github.io/scpm-repo/packages-bleeding.json

```

The Quick Start

Let's see how easy it is to start using `scpm`:

Update the local copy of the index database:

```

scpm update

```

Install the package of your choice along with all of its dependencies:

```

scpm install htop

```

List all of the installed packages:

```

scpm list

```

Uninstall a package:

```

scpm remove htop

```

What's This About

This proof-of-concept project tackles the issues related to `.git` clonings by using much lighter zip archives and `libcurl`, plus some JSON parsing and manifest tracking to properly build your sources and uninstall their traces on your system.

What's Next

Performance improvements and memory leaks fixes are the main focus of this project's future development. More package indexes and tweaks about the build flags will be added soon.
