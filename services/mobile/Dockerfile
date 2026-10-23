FROM rust:1.99.1-bookworm AS build
WORKDIR /build/relay
COPY relay/Cargo.toml relay/Cargo.lock ./
COPY relay/.cargo ./.cargo
COPY relay/src ./src
COPY crypto/Cargo.toml ../crypto/Cargo.toml
COPY crypto/src ../crypto/src
RUN cargo build --release --locked --jobs 2

FROM debian:bookworm-slim
ENV TMPDIR=/spool MALLOC_ARENA_MAX=2 MALLOC_MMAP_THRESHOLD_=131072 MALLOC_TRIM_THRESHOLD_=131072
RUN groupadd --gid 10001 relay && useradd --uid 10001 --gid 10001 --create-home relay \
    && mkdir /data /spool && chown relay:relay /data /spool && chmod 700 /data /spool
COPY --from=build /build/relay/target/release/zerus-relay /usr/local/bin/zerus-relay
RUN ln -s zerus-relay /usr/local/bin/zerus-mobile
USER 10001:10001
EXPOSE 8787
ENTRYPOINT ["zerus-relay", "--database", "/data/relay.sqlite3"]
CMD ["serve", "--host", "0.0.0.0"]
