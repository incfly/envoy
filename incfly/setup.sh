
#sdsserver() {
  #rm -rf /tmp/uds_path
  ## touch /tmp/uds_path
  #$GOPATH/src/github.com/incfly/sds-server/sds-server --uds-path /tmp/uds_path
#}

#sdsclient() {
 ## ./tools/stack_decode.py
  #bazel-bin/source/exe/envoy-static -c ../sds-test.yaml --component-log-level grpc:trace   --service-node 'abc'
#}

httpxs() {
  source incfly/setup.sh
  bazel-bin/source/exe/envoy-static -c incfly/httpx-server.yaml --base-id 2
}

httpxc() {
  source incfly/setup.sh
  bazel-bin/source/exe/envoy-static -c incfly/httpx-client.yaml --base-id 1
}

build() {
  bazel build //source/exe:envoy-static -c dbg
}

cfdump() {
  curl localhost:8001/config_dump
}

runtest() {
  #bazel test -c dbg \
    #//test/config_test:example_configs_test \
    #//test/integration:ads_integration_test \
    #//test/integration:integration_admin_test \
    #//test/server:listener_manager_impl_test  \
    #//test/server/config_validation:config_fuzz_test \
    #//test/server/config_validation:server_test 
}


buildwasm() {
  # somehow sha can't be extracted without setting image_id explcitly
  export IMAGE_NAME="piotrsikora/envoy" IMAGE_ID="9149ea02c06c7dd9fbadec4e72ab36fae6924c89" BAZEL_BUILD_EXTRA_OPTIONS='--define wasm=wavm'
  bash -x ./ci/run_envoy_docker.sh './ci/do_ci.sh bazel.release.server_only' 
}
