/// <reference path="./.sst/platform/config.d.ts" />

/**
 * ## AWS Cluster private service
 *
 * Adds a private load balancer to a service by setting the `loadBalancer.public` prop to
 * `false`.
 *
 * This allows you to create internal services that can only be accessed inside a VPC.
 */
export default $config({
  app(input) {
    return {
      name: "battlesnake-api",
      removal: input?.stage === "production" ? "retain" : "remove",
      home: "aws",
    };
  },
  async run() {
    const vpc = new sst.aws.Vpc("BattlesnakeVPC");
    const cluster = new sst.aws.Cluster("BattlesnakeCluster", { vpc });

    cluster.addService("Battlesnake", {
      loadBalancer: {
        ports: [{ listen: "80/http" }]
      },
      memory: '2 GB',
      cpu: '1 vCPU',
    });
  },
});
