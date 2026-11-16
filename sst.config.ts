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
    const controlToken = new sst.Secret("ControlToken");

    const service = cluster.addService("Battlesnake", {
      loadBalancer: {
        ports: [{ listen: "80/http" }]
      },
      memory: '2 GB',
      cpu: '1 vCPU',
      environment: {
        CONTROL_TOKEN: controlToken.value,
      },
    });

    // HTTPS for the pad: CloudFront forwards every path, the /ws upgrade included, to the
    // load balancer over plain HTTP. The engine keeps calling the load balancer directly.
    const router = new sst.aws.Router("BattlesnakeRouter", {
      transform: {
        // Every route is dynamic, so nothing is cached whatever the response headers say.
        cachePolicy: {
          defaultTtl: 0,
          maxTtl: 0,
          minTtl: 0,
          parametersInCacheKeyAndForwardedToOrigin: {
            cookiesConfig: { cookieBehavior: "none" },
            headersConfig: { headerBehavior: "none" },
            queryStringsConfig: { queryStringBehavior: "none" },
            enableAcceptEncodingBrotli: false,
            enableAcceptEncodingGzip: false,
          },
        },
      },
    });
    router.route("/", service.url);

    return {
      pad: router.url,
      engine: service.url,
    };
  },
});
