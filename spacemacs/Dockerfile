# docker.exe run --name spacemacs -e DISPLAY="host.docker.internal:0" -v C:\Users\minij\repositoris\meddia\:/mnt/workspace spacemacs/emacs25:develop
# Sets DISPLAY environment to Host, requires an X Server

FROM spacemacs/emacs25:develop

ENV DISPLAY="host.docker.internal:0"
ENV UNAME="spacemacser"
ENV UID="1000"

COPY .spacemacs "${UHOME}/.spacemacs"
COPY private "${UHOME}/.emacs.d/private"

RUN install-deps \
&& apt-get update && apt-get install \
    clang-3.7

VOLUME ["/MNT/WORKSPACE"]

