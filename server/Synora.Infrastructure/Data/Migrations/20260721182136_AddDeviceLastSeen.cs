using System;
using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Synora.Infrastructure.Data.Migrations
{
    /// <inheritdoc />
    public partial class AddDeviceLastSeen : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AddColumn<DateTimeOffset>(
                name: "LastSeenAt",
                table: "Devices",
                type: "timestamp with time zone",
                nullable: true);
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "LastSeenAt",
                table: "Devices");
        }
    }
}
