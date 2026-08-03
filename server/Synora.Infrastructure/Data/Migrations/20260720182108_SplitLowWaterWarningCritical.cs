using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Synora.Infrastructure.Data.Migrations
{
    /// <inheritdoc />
    public partial class SplitLowWaterWarningCritical : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.RenameColumn(
                name: "LowWaterThresholdPercent",
                table: "Devices",
                newName: "LowWaterWarningThresholdPercent");

            migrationBuilder.AddColumn<int>(
                name: "LowWaterCriticalThresholdPercent",
                table: "Devices",
                type: "integer",
                nullable: true);
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "LowWaterCriticalThresholdPercent",
                table: "Devices");

            migrationBuilder.RenameColumn(
                name: "LowWaterWarningThresholdPercent",
                table: "Devices",
                newName: "LowWaterThresholdPercent");
        }
    }
}
