FROM rust:1-slim-trixie AS build
WORKDIR /app
COPY server/ .
RUN cargo build --release --locked

FROM debian:trixie-slim
COPY --from=build /app/target/release/battlesnake-sk /usr/local/bin/battlesnake-sk
EXPOSE 80
CMD ["battlesnake-sk"]
