FROM mcr.microsoft.com/dotnet/sdk:9.0 AS build
WORKDIR /src

COPY Synora.Api/Synora.Api.csproj Synora.Api/
COPY Synora.Core/Synora.Core.csproj Synora.Core/
COPY Synora.Voice/Synora.Voice.csproj Synora.Voice/
COPY Synora.Protocol/Synora.Protocol.csproj Synora.Protocol/
COPY Synora.Infrastructure/Synora.Infrastructure.csproj Synora.Infrastructure/
RUN dotnet restore Synora.Api/Synora.Api.csproj

COPY . .
RUN dotnet publish Synora.Api/Synora.Api.csproj -c Release -o /app --no-restore

FROM mcr.microsoft.com/dotnet/aspnet:9.0 AS runtime
WORKDIR /app
COPY --from=build /app .

ENV ASPNETCORE_URLS=http://+:8080
EXPOSE 8080
ENTRYPOINT ["dotnet", "Synora.Api.dll"]
