FROM node:22-alpine AS web-build
WORKDIR /app

COPY package.json package-lock.json ./
RUN npm ci

COPY . .
# CMS API base as seen by the browser. On the shared domain Traefik strips /api
# and forwards to the Synora server, so /api/cms/* reaches its /cms/* routes.
ARG VITE_API_URL=/api/cms
ENV VITE_API_URL=$VITE_API_URL
RUN npm run build

# Static site only: the CMS API that used to run alongside nginx here
# (landing/server, Rust) now lives in the Synora .NET server under /cms --
# Traefik routes /api/cms/* and /uploads/* to it (traefik/dynamic.yml).
FROM nginx:1.27-alpine AS runtime

COPY --from=web-build /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf

EXPOSE 80
