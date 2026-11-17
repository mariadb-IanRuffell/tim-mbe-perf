# Standalone build + runtime image for wlib-mbe-sql and mbeperf (32-bit x86, Rocky Linux 9).
# No dependency on any other wlib image: the prebuilt wlib runtime libraries come from vendor/lib.
#
#   docker build --platform linux/amd64 -t wlib-mbe-sql .
#   docker run --rm --platform linux/amd64 --network <net> wlib-mbe-sql mbeperf -f mbeperf-my.mbe.ini --url mysql://user:pw@host/db

############ build stage
FROM --platform=linux/amd64 rockylinux:9 AS build
RUN dnf -y upgrade glib2 expat && dnf -y install dnf-plugins-core epel-release && dnf config-manager --set-enabled crb && \
    dnf -y install gcc make cmake pkgconf-pkg-config glibc-devel.i686 libgcc.i686 \
        glib2-devel.i686 mariadb-connector-c-devel.i686 librdkafka-devel.i686 \
        expat.i686 hiredis-devel.x86_64 && \
    dnf clean all

# Prebuilt wlib libraries (32-bit)
COPY vendor/lib/ /usr/local/lib/
# hiredis: headers from the x86_64 devel package (arch independent), library is the 32-bit one above
RUN ln -sf libhiredis.so.1 /usr/local/lib/libhiredis.so && \
    printf 'Name: hiredis\nDescription: hiredis\nVersion: 1.0.2\nLibs: -L/usr/local/lib -lhiredis\nCflags: -I/usr/include\n' > /usr/lib/pkgconfig/hiredis.pc
ENV PKG_CONFIG_PATH=/usr/lib/pkgconfig LD_LIBRARY_PATH=/usr/local/lib

COPY CMakeLists.txt /src/
COPY include /src/include
COPY src /src/src
COPY perf /src/perf
WORKDIR /src/build
RUN cmake .. -DWLIB_LIBRARY=/usr/local/lib/libwlib.so -DWLIB_GRPC_LIBRARY=/usr/local/lib/libwlib-grpc.so && make

############ runtime stage
FROM --platform=linux/amd64 rockylinux:9
RUN dnf -y upgrade glib2 expat && dnf -y install glibc.i686 glib2.i686 expat.i686 librdkafka.i686 mariadb-connector-c.i686 && \
    dnf clean all
COPY vendor/lib/ /usr/local/lib/
COPY --from=build /src/build/libwlib-mbe-sql.so /usr/local/lib/
COPY --from=build /src/build/mbeperf /usr/local/bin/mbeperf
COPY --from=build /src/build/mbeperf-my.mbe.ini /app/
COPY vendor/wlib.ini vendor/mbeperf.ini /app/
ENV LD_LIBRARY_PATH=/usr/local/lib
WORKDIR /app
CMD ["mbeperf", "--help"]
