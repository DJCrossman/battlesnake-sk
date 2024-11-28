FROM node:22-alpine
WORKDIR /app/

COPY . .

RUN apk --no-cache add curl

CMD yarn run start:prod
EXPOSE 80