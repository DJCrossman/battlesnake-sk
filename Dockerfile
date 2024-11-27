FROM node:22-alpine
WORKDIR /app/

COPY . .

CMD yarn run start:prod
EXPOSE 80