using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Synora.Infrastructure.Data.Migrations
{
    /// <inheritdoc />
    public partial class SwapTtsSpeakingRateForVoice : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "TtsSpeakingRate",
                table: "SystemSettings");

            migrationBuilder.AddColumn<string>(
                name: "TtsVoice",
                table: "SystemSettings",
                type: "text",
                nullable: false,
                defaultValue: "");

            migrationBuilder.UpdateData(
                table: "SystemSettings",
                keyColumn: "Id",
                keyValue: 1,
                column: "TtsVoice",
                value: "Trúc Ly");
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "TtsVoice",
                table: "SystemSettings");

            migrationBuilder.AddColumn<double>(
                name: "TtsSpeakingRate",
                table: "SystemSettings",
                type: "double precision",
                nullable: false,
                defaultValue: 0.0);

            migrationBuilder.UpdateData(
                table: "SystemSettings",
                keyColumn: "Id",
                keyValue: 1,
                column: "TtsSpeakingRate",
                value: 1.0);
        }
    }
}
