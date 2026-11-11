#ifndef __BR_PIPELINE0_H__
#define __BR_PIPELINE0_H__

int br_rx_loop(__attribute__((unused)) void *dummy);
int br_tx_loop(__attribute__((unused)) void *dummy);
int br_worker_loop(__attribute__((unused)) void *dummy);
#ifdef NIDT_TEST
int br_test_loop(__attribute__((unused)) void *dummy);
#endif
#ifdef ODCT_TEST
int br_odct_test_loop(__attribute__((unused)) void *dummy);
#endif

#endif  /* __BR_PIPELINE0_H__ */
