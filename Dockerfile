FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src/ src/
COPY tests/core.cpp tests/core.cpp
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4 && ./build/core_tests
FROM debian:bookworm-slim
RUN groupadd --gid 10001 cinder && useradd --uid 10001 --gid cinder --no-create-home cinder && mkdir /data && chown cinder:cinder /data
WORKDIR /app
COPY --from=build /src/build/cinder /app/cinder
COPY web/ /app/web/
USER cinder
EXPOSE 6380 8084
VOLUME /data
ENTRYPOINT ["/app/cinder"]
CMD ["--bind", "0.0.0.0", "--snapshot", "/data/cinder.snap"]
