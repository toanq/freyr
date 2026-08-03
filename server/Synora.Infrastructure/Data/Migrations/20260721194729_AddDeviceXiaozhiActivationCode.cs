using Microsoft.EntityFrameworkCore.Migrations;

#nullable disable

namespace Synora.Infrastructure.Data.Migrations
{
    /// <inheritdoc />
    public partial class AddDeviceXiaozhiActivationCode : Migration
    {
        /// <inheritdoc />
        protected override void Up(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.AddColumn<string>(
                name: "XiaozhiActivationCode",
                table: "Devices",
                type: "text",
                nullable: true);
        }

        /// <inheritdoc />
        protected override void Down(MigrationBuilder migrationBuilder)
        {
            migrationBuilder.DropColumn(
                name: "XiaozhiActivationCode",
                table: "Devices");
        }
    }
}
