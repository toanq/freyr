FROM node:22-alpine AS web-build
WORKDIR /app

COPY package.json package-lock.json ./
RUN npm ci

COPY . .
RUN npm run build

# Static site only: the CMS API that used to run alongside nginx here
# (landing/server, Rust) now lives in the Synora .NET server under /cms --
# nginx.conf proxies /api/* and /uploads/* to it.
FROM nginx:1.27-alpine AS runtime

COPY --from=web-build /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80
