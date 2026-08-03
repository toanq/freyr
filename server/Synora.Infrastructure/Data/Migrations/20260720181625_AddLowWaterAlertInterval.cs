using System;
using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Synora.Infrastructure.Data.Migrations
{
    /// <inheritdoc />
    public partial class AddLowWaterAlertInterval : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "LowWaterAlertSent",
                table: "Devices");

            migrationBuilder.AddColumn<int>(
                name: "LowWaterAlertIntervalMinutes",
                table: "Devices",
                type: "integer",
                nullable: true);

            migrationBuilder.AddColumn<DateTimeOffset>(
                name: "LowWaterLastAlertedAt",
                table: "Devices",
                type: "timestamp with time zone",
                nullable: true);
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "LowWaterAlertIntervalMinutes",
                table: "Devices");

            migrationBuilder.DropColumn(
                name: "LowWaterLastAlertedAt",
                table: "Devices");

            migrationBuilder.AddColumn<bool>(
                name: "LowWaterAlertSent",
                table: "Devices",
                type: "boolean",
                nullable: false,
                defaultValue: false);
        }
    }
}
