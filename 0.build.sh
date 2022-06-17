#!/bin/bash

export FORCE_UNSAFE_CONFIGURE=1

rpmbuild -ba SPECS/coreutils.spec  --nocheck
