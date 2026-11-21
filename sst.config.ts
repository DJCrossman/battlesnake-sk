/// <reference path="./.sst/platform/config.d.ts" />

/**
 * ## One EC2 instance behind a fixed IP
 *
 * The server keeps all its state in memory, so it runs as one container on one small EC2 instance
 * with an Elastic IP. The engine calls that IP directly; CloudFront in front of it gives the pad
 * and the WiFi board HTTPS.
 */
export default $config({
  app(input) {
    return {
      name: "battlesnake-api",
      removal: input?.stage === "production" ? "retain" : "remove",
      home: "aws",
      providers: { "docker-build": "0.0.23" },
    };
  },
  async run() {
    const vpc = new sst.aws.Vpc("BattlesnakeVPC");
    const controlToken = new sst.Secret("ControlToken");

    const repository = new aws.ecr.Repository("BattlesnakeRepository", {
      // ECR names must be lowercase, which the generated one is not.
      name: `${$app.name}-${$app.stage}`,
      // Lets a removed stage take its images with it.
      forceDelete: true,
    });
    const registry = aws.ecr.getAuthorizationTokenOutput({ registryId: repository.registryId });
    const image = new dockerbuild.Image("BattlesnakeImage", {
      context: { location: $cli.paths.root },
      dockerfile: { location: `${$cli.paths.root}/Dockerfile` },
      platforms: ["linux/arm64"],
      tags: [$interpolate`${repository.repositoryUrl}:latest`],
      registries: [
        {
          address: registry.proxyEndpoint,
          username: registry.userName,
          password: $util.secret(registry.password),
        },
      ],
      // The registry cache keeps the digest the same when the server has not changed, so a deploy
      // without server changes leaves the instance alone, as SST does for its own services.
      cacheFrom: [{ registry: { ref: $interpolate`${repository.repositoryUrl}:cache` } }],
      cacheTo: [
        {
          registry: {
            ref: $interpolate`${repository.repositoryUrl}:cache`,
            imageManifest: true,
            ociMediaTypes: true,
            mode: "max",
          },
        },
      ],
      push: true,
    });

    const role = new aws.iam.Role("BattlesnakeInstanceRole", {
      assumeRolePolicy: aws.iam.assumeRolePolicyForPrincipal({ Service: "ec2.amazonaws.com" }),
    });
    new aws.iam.RolePolicyAttachment("BattlesnakeInstanceEcr", {
      role: role.name,
      policyArn: aws.iam.ManagedPolicy.AmazonEC2ContainerRegistryReadOnly,
    });
    // Session Manager instead of SSH, so port 22 stays closed.
    new aws.iam.RolePolicyAttachment("BattlesnakeInstanceSsm", {
      role: role.name,
      policyArn: aws.iam.ManagedPolicy.AmazonSSMManagedInstanceCore,
    });

    const instance = new aws.ec2.Instance(
      "BattlesnakeInstance",
      {
        instanceType: "t4g.micro",
        ami: aws.ssm.getParameterOutput({
          name: "/aws/service/ami-amazon-linux-latest/al2023-ami-kernel-default-arm64",
        }).value,
        // A public subnet gives the instance a route out to ECR without a NAT gateway, and a
        // public address of its own until the Elastic IP is attached.
        subnetId: vpc.publicSubnets.apply((subnets) => subnets[0]),
        vpcSecurityGroupIds: [
          new aws.ec2.SecurityGroup("BattlesnakeSecurityGroup", {
            vpcId: vpc.id,
            ingress: [{ protocol: "tcp", fromPort: 80, toPort: 80, cidrBlocks: ["0.0.0.0/0"] }],
            egress: [{ protocol: "-1", fromPort: 0, toPort: 0, cidrBlocks: ["0.0.0.0/0"] }],
          }).id,
        ],
        iamInstanceProfile: new aws.iam.InstanceProfile("BattlesnakeInstanceProfile", { role: role.name }),
        // Every download retries: the instance's public address changes when the Elastic IP
        // arrives, which can cut one off, and the role's credentials can lag behind the boot.
        userData: $interpolate`#!/bin/bash
set -uo pipefail
until dnf install -y docker; do sleep 5; done
systemctl enable --now docker
until aws ecr get-login-password --region ${aws.getRegionOutput().name} | docker login --username AWS --password-stdin ${registry.proxyEndpoint}; do
  sleep 5
done
until docker pull ${image.ref}; do sleep 5; done
docker run -d --restart=always --network host -e CONTROL_TOKEN='${controlToken.value}' ${image.ref}
`,
        // A new image means new user data, and so a new instance.
        userDataReplaceOnChange: true,
      },
      // A newer Amazon Linux release alone should not take the snake offline, so the instance
      // keeps the release it was first built from. Drop this line for one deploy to move it on.
      { ignoreChanges: ["ami"] },
    );

    // The address survives a new instance, so the engine registration and the CloudFront origin
    // stay valid.
    const ip = new aws.ec2.Eip("BattlesnakeIp", { domain: "vpc", instance: instance.id });

    // HTTPS for the pad: CloudFront forwards every path, the /ws upgrade included, to the
    // instance over plain HTTP. The engine keeps calling the instance directly.
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
    // CloudFront origins must be DNS names; the Elastic IP's public name follows the address.
    router.route("/", $interpolate`http://${ip.publicDns}`);

    return {
      pad: router.url,
      engine: $interpolate`http://${ip.publicIp}`,
    };
  },
});
