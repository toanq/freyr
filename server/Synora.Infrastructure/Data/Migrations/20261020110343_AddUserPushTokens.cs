using System;
using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Synora.Infrastructure.Data.Migrations
{
    /// <inheritdoc />
    public partial class AddUserPushTokens : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.CreateTable(
                name: "PushTokens",
                columns: table => new
                {
                    Id = table.Column<Guid>(type: "uuid", nullable: false),
                    UserId = table.Column<Guid>(type: "uuid", nullable: false),
                    Token = table.Column<string>(type: "text", nullable: false),
                    Platform = table.Column<string>(type: "character varying(16)", maxLength: 16, nullable: false),
                    CreatedAt = table.Column<DateTimeOffset>(type: "timestamp with time zone", nullable: false),
                    UpdatedAt = table.Column<DateTimeOffset>(type: "timestamp with time zone", nullable: false)
                },
                constraints: table =>
                {
                    table.PrimaryKey("PK_PushTokens", x => x.Id);
                    table.ForeignKey(
                        name: "FK_PushTokens_Users_UserId",
                        column: x => x.UserId,
                        principalTable: "Users",
                        principalColumn: "Id",
                        onDelete: ReferentialAction.Cascade);
                });

            migrationBuilder.CreateIndex(
                name: "IX_PushTokens_Token",
                table: "PushTokens",
                column: "Token",
                unique: true);

            migrationBuilder.CreateIndex(
                name: "IX_PushTokens_UserId",
                table: "PushTokens",
                column: "UserId");

            // Carry over each user's existing (phone app) token before dropping the
            // column. Tokens are unique now, so a token shared by two users keeps one.
            migrationBuilder.Sql("""
                INSERT INTO "PushTokens" ("Id", "UserId", "Token", "Platform", "CreatedAt", "UpdatedAt")
                SELECT DISTINCT ON ("FcmToken") gen_random_uuid(), "Id", "FcmToken", 'android', now(), now()
                FROM "Users"
                WHERE "FcmToken" IS NOT NULL AND "FcmToken" <> ''
                ORDER BY "FcmToken", "CreatedAt" DESC;
                """);

            migrationBuilder.DropColumn(
                name: "FcmToken",
                table: "Users");
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AddColumn<string>(
                name: "FcmToken",
                table: "Users",
                type: "text",
                nullable: true);

            // Single-token column again: keep each user's most recent app token.
            migrationBuilder.Sql("""
                UPDATE "Users" u SET "FcmToken" = t."Token"
                FROM (
                    SELECT DISTINCT ON ("UserId") "UserId", "Token"
                    FROM "PushTokens"
                    WHERE "Platform" <> 'web'
                    ORDER BY "UserId", "UpdatedAt" DESC
                ) t
                WHERE u."Id" = t."UserId";
                """);

            migrationBuilder.DropTable(
                name: "PushTokens");
        }
    }
}
